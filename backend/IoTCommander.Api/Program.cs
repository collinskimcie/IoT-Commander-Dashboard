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
    terminal.LatestReadings = MergeLatest(terminal.LatestReadings, request.Readings, receivedAt);
    database.TelemetryRecords.AddRange(request.Readings.Select(reading => new TelemetryRecord
    {
        TerminalId = terminalId,
        Name = reading.Name,
        Value = reading.Value,
        Unit = reading.Unit ?? string.Empty,
        ReceivedAt = receivedAt
    }));
    await database.SaveChangesAsync(cancellationToken);
    return Results.Accepted($"/api/v1/terminals/{terminalId}/telemetry");
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

static string MergeLatest(string? existing, IEnumerable<ReadingIngestRequest> incoming, DateTimeOffset at)
{
    var map = ParseLatest(existing).ToDictionary(item => item.Name);
    foreach (var reading in incoming)
    {
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
