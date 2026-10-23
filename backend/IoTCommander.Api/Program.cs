using System.Security.Cryptography;
using System.Text;
using IoTCommander.Api;
using Microsoft.EntityFrameworkCore;

const int MaxReadingsPerPost = 5; // keep in sync with kMaxTelemetryReadings in Domain.h

const int HeartbeatIntervalSeconds = 30; // the firmware sends a heartbeat every 30 s

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.ConfigureKestrel(options => options.Limits.MaxRequestBodySize = 8192);

var connectionString = builder.Configuration.GetConnectionString("Database")
    ?? throw new InvalidOperationException("ConnectionStrings:Database must be configured.");
var apiKey = builder.Configuration["IOT_API_KEY"]
    ?? throw new InvalidOperationException("IOT_API_KEY must be configured.");
if (apiKey.Length < 32)
{
    throw new InvalidOperationException("IOT_API_KEY must contain at least 32 characters.");
}

builder.Services.AddDbContext<CommanderDbContext>(options => options.UseNpgsql(connectionString));

var app = builder.Build();

app.Use(async (context, next) =>
{
    if (context.Request.Path.StartsWithSegments("/api/v1"))
    {
        var provided = context.Request.Headers["X-Api-Key"].ToString();
        var providedBytes = Encoding.UTF8.GetBytes(provided);
        var expectedBytes = Encoding.UTF8.GetBytes(apiKey);
        if (!CryptographicOperations.FixedTimeEquals(providedBytes, expectedBytes))
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            await context.Response.WriteAsJsonAsync(new { error = "A valid X-Api-Key header is required." });
            return;
        }
    }
    await next();
});

app.MapGet("/health", () => Results.Ok(new { status = "ok" }));

app.MapPost("/api/v1/terminals/{terminalId}/connections", async (
    string terminalId,
    ConnectionIngestRequest request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId) || !IsValidConnection(request))
    {
        return Results.ValidationProblem(new Dictionary<string, string[]>
        {
            ["request"] = ["Terminal ID, connection type, state, or details are invalid."]
        });
    }

    var terminal = await GetOrCreateTerminal(database, terminalId, cancellationToken);
    terminal.LastSeenAt = DateTimeOffset.UtcNow;
    database.ConnectionEvents.Add(new ConnectionEventRecord
    {
        TerminalId = terminalId,
        Type = request.Type,
        State = request.State,
        Details = request.Details,
        OccurredAt = DateTimeOffset.UtcNow
    });
    await database.SaveChangesAsync(cancellationToken);
    return Results.Accepted($"/api/v1/terminals/{terminalId}/connections");
});

app.MapPost("/api/v1/terminals/{terminalId}/telemetry", async (
    string terminalId,
    TelemetryIngestRequest request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId) ||
        request.Readings is null ||
        request.Readings.Count < 1 || request.Readings.Count > MaxReadingsPerPost ||
        request.Readings.Any(reading =>
            string.IsNullOrWhiteSpace(reading.Name) ||
            reading.Name.Length > 24 ||
            reading.Unit?.Length > 8 ||
            !double.IsFinite(reading.Value)))
    {
        return Results.ValidationProblem(new Dictionary<string, string[]>
        {
            ["readings"] = [$"Provide between 1 and {MaxReadingsPerPost} named, finite numeric readings."]
        });
    }

    var terminal = await GetOrCreateTerminal(database, terminalId, cancellationToken);
    terminal.LastSeenAt = DateTimeOffset.UtcNow;
    var receivedAt = DateTimeOffset.UtcNow;

    // Store-and-forward: a reading may carry the time it was observed. Trust it only when plausible.
    (DateTimeOffset At, bool Observed) ResolveTime(ReadingIngestRequest reading)
    {
        if (reading.ObservedAt is { } seconds && seconds > 0 && seconds < 253402300799)
        {
            var observed = DateTimeOffset.FromUnixTimeSeconds(seconds);
            if (observed >= receivedAt.AddDays(-7) && observed <= receivedAt.AddMinutes(5))
            {
                return (observed > receivedAt ? receivedAt : observed, true);
            }
        }
        return (receivedAt, false);
    }

    var stamped = request.Readings.Select(reading => (Reading: reading, Time: ResolveTime(reading))).ToList();

    // A retry after a lost response must not store the same reading twice.
    var observedTimes = stamped.Where(item => item.Time.Observed).Select(item => item.Time.At).Distinct().ToList();
    var alreadyStored = new HashSet<(string, DateTimeOffset)>();
    if (observedTimes.Count > 0)
    {
        var rows = await Microsoft.EntityFrameworkCore.EntityFrameworkQueryableExtensions.ToListAsync(
            database.TelemetryRecords
                .Where(item => item.TerminalId == terminalId && observedTimes.Contains(item.ReceivedAt))
                .Select(item => new { item.Name, item.ReceivedAt }),
            cancellationToken);
        foreach (var row in rows)
        {
            alreadyStored.Add((row.Name, row.ReceivedAt));
        }
    }

    await RaiseAlerts(database, terminalId, terminal.LatestReadings,
        stamped.Select(item => (item.Reading, item.Time.At)).ToList(), receivedAt, cancellationToken);
    terminal.LatestReadings = MergeLatest(terminal.LatestReadings, stamped.Select(item => (item.Reading, item.Time.At)));
    database.TelemetryRecords.AddRange(stamped
        .Where(item => !alreadyStored.Contains((item.Reading.Name, item.Time.At)))
        .Select(item => new TelemetryRecord
        {
            TerminalId = terminalId,
            Name = item.Reading.Name,
            Value = item.Reading.Value,
            Unit = item.Reading.Unit ?? string.Empty,
            ReceivedAt = item.Time.At
        }));
    await database.SaveChangesAsync(cancellationToken);
    return Results.Accepted($"/api/v1/terminals/{terminalId}/telemetry");
});

app.MapGet("/api/v1/alert-rules", async (
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
    Results.Ok(await database.AlertRules
        .AsNoTracking()
        .OrderBy(rule => rule.Id)
        .ToListAsync(cancellationToken)));

app.MapPost("/api/v1/alert-rules", async (
    AlertRuleRequest request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    var problem = AlertLogic.Validate(request);
    if (problem is not null)
    {
        return Results.BadRequest(new { error = problem });
    }

    if (await database.AlertRules.CountAsync(cancellationToken) >= AlertLogic.MaxRules)
    {
        return Results.BadRequest(new { error = $"At most {AlertLogic.MaxRules} alert rules are allowed." });
    }

    var rule = new AlertRuleRecord
    {
        TerminalId = AlertLogic.NormalizeTerminal(request.TerminalId),
        Metric = request.Metric.Trim(),
        Comparison = request.Comparison,
        Threshold = request.Threshold,
        Enabled = request.Enabled ?? true,
        CreatedAt = DateTimeOffset.UtcNow
    };
    database.AlertRules.Add(rule);
    await database.SaveChangesAsync(cancellationToken);
    return Results.Created($"/api/v1/alert-rules/{rule.Id}", rule);
});

app.MapPut("/api/v1/alert-rules/{id:long}", async (
    long id,
    AlertRuleRequest request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    var problem = AlertLogic.Validate(request);
    if (problem is not null)
    {
        return Results.BadRequest(new { error = problem });
    }

    var rule = await database.AlertRules.FindAsync([id], cancellationToken);
    if (rule is null)
    {
        return Results.NotFound();
    }

    rule.TerminalId = AlertLogic.NormalizeTerminal(request.TerminalId);
    rule.Metric = request.Metric.Trim();
    rule.Comparison = request.Comparison;
    rule.Threshold = request.Threshold;
    rule.Enabled = request.Enabled ?? rule.Enabled;
    await database.SaveChangesAsync(cancellationToken);
    return Results.Ok(rule);
});

app.MapDelete("/api/v1/alert-rules/{id:long}", async (
    long id,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    var rule = await database.AlertRules.FindAsync([id], cancellationToken);
    if (rule is null)
    {
        return Results.NotFound();
    }

    database.AlertRules.Remove(rule);
    await database.SaveChangesAsync(cancellationToken);
    return Results.NoContent();
});

app.MapGet("/api/v1/alerts", async (
    int? limit,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
    Results.Ok(await database.AlertEvents
        .AsNoTracking()
        .OrderByDescending(item => item.Id)
        .Take(Math.Clamp(limit ?? 30, 1, 100))
        .ToListAsync(cancellationToken)));

// Polled by the terminal. Without afterId it only reports the newest id, so a fresh boot never replays old alerts.
app.MapGet("/api/v1/terminals/{terminalId}/alerts", async (
    string terminalId,
    long? afterId,
    int? limit,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new { error = "Terminal ID must be a 12-character hexadecimal ID." });
    }

    var events = database.AlertEvents.AsNoTracking().Where(item => item.TerminalId == terminalId);
    var latestId = await events.MaxAsync(item => (long?)item.Id, cancellationToken) ?? 0;
    if (afterId is null)
    {
        return Results.Ok(new { latestId, alerts = Array.Empty<object>() });
    }

    var alerts = await events
        .Where(item => item.Id > afterId.Value)
        .OrderBy(item => item.Id)
        .Take(Math.Clamp(limit ?? 5, 1, 20))
        .Select(item => new { item.Id, item.Message, item.OccurredAt })
        .ToListAsync(cancellationToken);
    return Results.Ok(new { latestId, alerts });
});

app.MapPost("/api/v1/terminals/{terminalId}/heartbeat", async (
    string terminalId,
    HeartbeatIngestRequest? request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new
        {
            error = "Terminal ID must be a 12-character hexadecimal ID."
        });
    }

    var terminal = await GetOrCreateTerminal(database, terminalId, cancellationToken);
    var heartbeatAt = DateTimeOffset.UtcNow;
    terminal.LastSeenAt = heartbeatAt;
    if (request is not null)
    {
        if (request.UptimeSeconds.HasValue)
        {
            // Uptime going backwards (or a first report) means the terminal rebooted.
            if (!terminal.UptimeSeconds.HasValue || request.UptimeSeconds.Value < terminal.UptimeSeconds.Value)
            {
                terminal.BootCount++;
            }
            terminal.UptimeSeconds = request.UptimeSeconds;
            terminal.LastBootAt = heartbeatAt - TimeSpan.FromSeconds(request.UptimeSeconds.Value);
        }
        terminal.Firmware = Truncate(request.Fw, 24);
        terminal.HeapFree = request.HeapFree;
        terminal.HeapMaxBlock = request.HeapMaxBlock;
        terminal.PsramFree = request.PsramFree;
        terminal.Rssi = request.Rssi;
        terminal.ResetReason = Truncate(request.ResetReason, 16);
        database.HeartbeatRecords.Add(new HeartbeatRecord
        {
            TerminalId = terminalId,
            ReceivedAt = heartbeatAt,
            Rssi = request.Rssi,
            HeapFree = request.HeapFree
        });
    }
    await database.SaveChangesAsync(cancellationToken);

    return Results.Accepted($"/api/v1/terminals/{terminalId}/heartbeat");
});

app.MapGet("/api/v1/terminals", async (
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    var now = DateTimeOffset.UtcNow;
    var since = now.AddHours(-24);
    var heartbeatCounts = await database.HeartbeatRecords
        .AsNoTracking()
        .Where(item => item.ReceivedAt >= since)
        .GroupBy(item => item.TerminalId)
        .Select(group => new { TerminalId = group.Key, Count = group.Count() })
        .ToDictionaryAsync(item => item.TerminalId, item => item.Count, cancellationToken);
    var records = await database.Terminals
        .AsNoTracking()
        .OrderByDescending(terminal => terminal.LastSeenAt)
        .ToListAsync(cancellationToken);

    var terminals = records.Select(terminal =>
    {
        var age = now - terminal.LastSeenAt;
        var ageSeconds = Math.Max(0, (long)age.TotalSeconds);
        var status = ageSeconds <= 60
            ? "online"
            : ageSeconds <= 180
                ? "stale"
                : "offline";

        heartbeatCounts.TryGetValue(terminal.Id, out var beats);
        var windowStart = terminal.FirstSeenAt > since ? terminal.FirstSeenAt : since;
        var windowSeconds = Math.Max(HeartbeatIntervalSeconds, (now - windowStart).TotalSeconds);
        var availability = Math.Min(100.0, Math.Round(beats * HeartbeatIntervalSeconds / windowSeconds * 100.0, 1));

        return new
        {
            terminal.Id,
            terminal.Name,
            terminal.GroupName,
            terminal.FirstSeenAt,
            terminal.LastSeenAt,
            status,
            lastSeenAgeSeconds = ageSeconds,
            terminal.Firmware,
            terminal.UptimeSeconds,
            terminal.LastBootAt,
            terminal.BootCount,
            terminal.ResetReason,
            terminal.HeapFree,
            terminal.HeapMaxBlock,
            terminal.PsramFree,
            terminal.Rssi,
            availability24h = availability,
            latest = ParseLatest(terminal.LatestReadings)
        };
    });

    return Results.Ok(terminals);
});

app.MapGet("/api/v1/terminals/{terminalId}/connections", async (
    string terminalId,
    int? limit,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new { error = "Terminal ID must be a 12-character hexadecimal ID." });
    }
    var count = Math.Clamp(limit ?? 50, 1, 200);
    var events = await database.ConnectionEvents
        .Where(item => item.TerminalId == terminalId)
        .OrderByDescending(item => item.OccurredAt)
        .Take(count)
        .Select(item => new { item.Type, item.State, item.Details, item.OccurredAt })
        .ToListAsync(cancellationToken);
    return Results.Ok(events);
});

app.MapGet("/api/v1/terminals/sparklines", async (
    int? minutes,
    int? points,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    var windowMinutes = Math.Clamp(minutes ?? 60, 5, 1440);
    var bucketCount = Math.Clamp(points ?? 30, 5, 60);
    var until = DateTimeOffset.UtcNow;
    var since = until.AddMinutes(-windowMinutes);
    var bucketMilliseconds = windowMinutes * 60_000.0 / bucketCount;

    // Newest rows first, so the cap drops the oldest history rather than the latest values.
    var rows = await database.TelemetryRecords
        .AsNoTracking()
        .Where(item => item.ReceivedAt >= since && item.ReceivedAt <= until)
        .OrderByDescending(item => item.ReceivedAt)
        .Select(item => new { item.TerminalId, item.Name, item.Value, item.ReceivedAt })
        .Take(20000)
        .ToListAsync(cancellationToken);

    var result = rows
        .GroupBy(item => item.TerminalId)
        .ToDictionary(
            terminal => terminal.Key,
            terminal => terminal
                .GroupBy(item => item.Name)
                .OrderByDescending(metric => metric.Count())
                .ThenBy(metric => metric.Key)
                .Take(3)
                .ToDictionary(
                    metric => metric.Key,
                    metric => metric
                        .GroupBy(item => Math.Min(bucketCount - 1,
                            (int)((item.ReceivedAt - since).TotalMilliseconds / bucketMilliseconds)))
                        .OrderBy(bucket => bucket.Key)
                        .Select(bucket => Math.Round(bucket.Average(item => item.Value), 3))
                        .ToList()));

    return Results.Ok(result);
});

app.MapGet("/api/v1/terminals/{terminalId}/telemetry", async (
    string terminalId,
    int? limit,
    DateTimeOffset? from,
    DateTimeOffset? to,
    bool? ascending,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new
        {
            error = "Terminal ID must be a 12-character hexadecimal ID."
        });
    }

    if (from.HasValue && to.HasValue && from.Value > to.Value)
    {
        return Results.BadRequest(new
        {
            error = "'from' must be earlier than or equal to 'to'."
        });
    }

    var count = Math.Clamp(limit ?? 100, 1, 500);

    IQueryable<TelemetryRecord> query = database.TelemetryRecords
        .AsNoTracking()
        .Where(item => item.TerminalId == terminalId);

    if (from.HasValue)
    {
        query = query.Where(item => item.ReceivedAt >= from.Value);
    }

    if (to.HasValue)
    {
        query = query.Where(item => item.ReceivedAt <= to.Value);
    }

    var records = await query
        .OrderByDescending(item => item.ReceivedAt)
        .ThenByDescending(item => item.Id)
        .Take(count)
        .ToListAsync(cancellationToken);

    if (ascending == true)
    {
        records.Reverse();
    }

    var readings = records.Select(item => new
    {
        item.Name,
        item.Value,
        item.Unit,
        item.ReceivedAt
    }).ToList();

    return Results.Ok(readings);
});

app.MapPut("/api/v1/terminals/{terminalId}", async (
    string terminalId,
    TerminalUpdateRequest request,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId) ||
        (request.Name?.Length ?? 0) > 40 ||
        (request.GroupName?.Length ?? 0) > 40)
    {
        return Results.ValidationProblem(new Dictionary<string, string[]>
        {
            ["request"] = ["Terminal ID is invalid, or name or group is longer than 40 characters."]
        });
    }

    var terminal = await database.Terminals.FindAsync([terminalId], cancellationToken);
    if (terminal is null)
    {
        return Results.NotFound();
    }
    terminal.Name = string.IsNullOrWhiteSpace(request.Name) ? null : request.Name.Trim();
    terminal.GroupName = string.IsNullOrWhiteSpace(request.GroupName) ? null : request.GroupName.Trim();
    await database.SaveChangesAsync(cancellationToken);
    return Results.NoContent();
});

app.MapGet("/api/v1/terminals/{terminalId}/health", async (
    string terminalId,
    int? hours,
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new { error = "Terminal ID must be a 12-character hexadecimal ID." });
    }
    var since = DateTimeOffset.UtcNow.AddHours(-Math.Clamp(hours ?? 24, 1, 168));
    var rows = await database.HeartbeatRecords
        .AsNoTracking()
        .Where(item => item.TerminalId == terminalId && item.ReceivedAt >= since)
        .OrderBy(item => item.ReceivedAt)
        .Select(item => new { item.ReceivedAt, item.Rssi, item.HeapFree })
        .ToListAsync(cancellationToken);
    var stride = Math.Max(1, rows.Count / 500);
    return Results.Ok(rows.Where((_, index) => index % stride == 0));
});

using (var scope = app.Services.CreateScope())
{
    var database = scope.ServiceProvider.GetRequiredService<CommanderDbContext>();
    await database.Database.EnsureCreatedAsync();
    // Bridge until EF migrations are introduced: adds new columns/tables to an existing database.
    await database.Database.ExecuteSqlRawAsync("""
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "Name" varchar(40) NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "GroupName" varchar(40) NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "Firmware" varchar(24) NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "UptimeSeconds" bigint NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "HeapFree" bigint NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "HeapMaxBlock" bigint NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "PsramFree" bigint NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "Rssi" integer NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "ResetReason" varchar(16) NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "LastBootAt" timestamp with time zone NULL;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "BootCount" integer NOT NULL DEFAULT 0;
        ALTER TABLE "Terminals" ADD COLUMN IF NOT EXISTS "LatestReadings" text NULL;
        CREATE TABLE IF NOT EXISTS "HeartbeatRecords" (
            "Id" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
            "TerminalId" varchar(12) NOT NULL REFERENCES "Terminals" ("Id") ON DELETE CASCADE,
            "ReceivedAt" timestamp with time zone NOT NULL,
            "Rssi" integer NULL,
            "HeapFree" bigint NULL
        );
        CREATE INDEX IF NOT EXISTS "IX_HeartbeatRecords_TerminalId_ReceivedAt"
            ON "HeartbeatRecords" ("TerminalId", "ReceivedAt");
        CREATE TABLE IF NOT EXISTS "AlertRules" (
            "Id" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
            "TerminalId" varchar(12) NULL,
            "Metric" varchar(24) NOT NULL,
            "Comparison" varchar(1) NOT NULL,
            "Threshold" double precision NOT NULL,
            "Enabled" boolean NOT NULL DEFAULT TRUE,
            "CreatedAt" timestamp with time zone NOT NULL
        );
        CREATE TABLE IF NOT EXISTS "AlertEvents" (
            "Id" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
            "TerminalId" varchar(12) NOT NULL,
            "RuleId" bigint NOT NULL,
            "Metric" varchar(24) NOT NULL,
            "Value" double precision NOT NULL,
            "Message" varchar(160) NOT NULL,
            "OccurredAt" timestamp with time zone NOT NULL
        );
        CREATE INDEX IF NOT EXISTS "IX_AlertEvents_TerminalId_Id"
            ON "AlertEvents" ("TerminalId", "Id");
        """);
}

await app.RunAsync();

static async Task<TerminalRecord> GetOrCreateTerminal(
    CommanderDbContext database,
    string terminalId,
    CancellationToken cancellationToken)
{
    var terminal = await database.Terminals.FindAsync([terminalId], cancellationToken);
    if (terminal is not null)
    {
        return terminal;
    }

    terminal = new TerminalRecord
    {
        Id = terminalId,
        FirstSeenAt = DateTimeOffset.UtcNow,
        LastSeenAt = DateTimeOffset.UtcNow
    };
    database.Terminals.Add(terminal);
    return terminal;
}

// Fires an alert when a reading crosses a rule's limit. Replays and stale readings never alert.
static async Task RaiseAlerts(
    CommanderDbContext database,
    string terminalId,
    string? previousLatestJson,
    IReadOnlyList<(ReadingIngestRequest Reading, DateTimeOffset At)> incoming,
    DateTimeOffset receivedAt,
    CancellationToken cancellationToken)
{
    var rules = await database.AlertRules
        .AsNoTracking()
        .Where(rule => rule.Enabled && (rule.TerminalId == null || rule.TerminalId == terminalId))
        .ToListAsync(cancellationToken);
    if (rules.Count == 0)
    {
        return;
    }

    var previous = ParseLatest(previousLatestJson).ToDictionary(item => item.Name);
    var cooldownStart = receivedAt.AddMinutes(-AlertLogic.CooldownMinutes);
    var coolingRows = await database.AlertEvents
        .AsNoTracking()
        .Where(item => item.TerminalId == terminalId && item.OccurredAt >= cooldownStart)
        .Select(item => item.RuleId)
        .ToListAsync(cancellationToken);
    var cooling = coolingRows.ToHashSet();

    foreach (var (reading, at) in incoming)
    {
        previous.TryGetValue(reading.Name, out var before);
        var olderThanStored = before is not null && before.At > at;
        var fresh = at >= receivedAt.AddMinutes(-AlertLogic.FreshMinutes);

        if (!olderThanStored && fresh)
        {
            foreach (var rule in rules.Where(item => item.Metric == reading.Name))
            {
                var nowMet = AlertLogic.ConditionMet(rule.Comparison, reading.Value, rule.Threshold);
                var wasMet = before is not null &&
                    AlertLogic.ConditionMet(rule.Comparison, before.Value, rule.Threshold);
                if (!nowMet || wasMet || cooling.Contains(rule.Id))
                {
                    continue;
                }

                database.AlertEvents.Add(new AlertEventRecord
                {
                    TerminalId = terminalId,
                    RuleId = rule.Id,
                    Metric = reading.Name,
                    Value = reading.Value,
                    Message = AlertLogic.Describe(
                        reading.Name, rule.Comparison, rule.Threshold, reading.Value, reading.Unit ?? string.Empty),
                    OccurredAt = at
                });
                cooling.Add(rule.Id);
            }
        }

        if (!olderThanStored)
        {
            previous[reading.Name] = new LatestReading(reading.Name, reading.Value, reading.Unit ?? string.Empty, at);
        }
    }
}

static string? Truncate(string? value, int maxLength) =>
    value is null ? null : value.Length <= maxLength ? value : value[..maxLength];

static List<LatestReading> ParseLatest(string? json)
{
    if (string.IsNullOrEmpty(json))
    {
        return [];
    }
    try
    {
        return System.Text.Json.JsonSerializer.Deserialize<List<LatestReading>>(json) ?? [];
    }
    catch (System.Text.Json.JsonException)
    {
        return [];
    }
}

static string MergeLatest(string? existing, IEnumerable<(ReadingIngestRequest Reading, DateTimeOffset At)> incoming)
{
    var map = ParseLatest(existing).ToDictionary(item => item.Name);
    foreach (var (reading, at) in incoming)
    {
        // A replayed older reading must not replace a fresher stored value.
        if (map.TryGetValue(reading.Name, out var current) && current.At > at)
        {
            continue;
        }
        map[reading.Name] = new LatestReading(reading.Name, reading.Value, reading.Unit ?? string.Empty, at);
    }
    return System.Text.Json.JsonSerializer.Serialize(map.Values.OrderBy(item => item.Name).Take(8));
}

static bool IsValidTerminalId(string terminalId) =>
    terminalId.Length == 12 && terminalId.All(Uri.IsHexDigit);

static bool IsValidConnection(ConnectionIngestRequest request) =>
    request.Type is "wifi" or "mqtt" &&
    request.State is "connected" or "disconnected" &&
    request.Details is not null &&
    request.Details.Length <= 160;
