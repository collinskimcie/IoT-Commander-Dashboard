using System.Security.Cryptography;
using System.Text;
using IoTCommander.Api;
using Microsoft.EntityFrameworkCore;

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
        request.Readings.Count is < 1 or > 4 ||
        request.Readings.Any(reading =>
            string.IsNullOrWhiteSpace(reading.Name) ||
            reading.Name.Length > 24 ||
            reading.Unit?.Length > 8 ||
            !double.IsFinite(reading.Value)))
    {
        return Results.ValidationProblem(new Dictionary<string, string[]>
        {
            ["readings"] = ["Provide between one and four named, finite numeric readings."]
        });
    }

    var terminal = await GetOrCreateTerminal(database, terminalId, cancellationToken);
    terminal.LastSeenAt = DateTimeOffset.UtcNow;
    var receivedAt = DateTimeOffset.UtcNow;
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

app.MapGet("/api/v1/terminals", async (CommanderDbContext database, CancellationToken cancellationToken) =>
{
    var terminals = await database.Terminals
        .OrderByDescending(terminal => terminal.LastSeenAt)
        .Select(terminal => new { terminal.Id, terminal.FirstSeenAt, terminal.LastSeenAt })
        .ToListAsync(cancellationToken);
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
    CommanderDbContext database,
    CancellationToken cancellationToken) =>
{
    if (!IsValidTerminalId(terminalId))
    {
        return Results.BadRequest(new { error = "Terminal ID must be a 12-character hexadecimal ID." });
    }
    var count = Math.Clamp(limit ?? 100, 1, 500);
    var readings = await database.TelemetryRecords
        .Where(item => item.TerminalId == terminalId)
        .OrderByDescending(item => item.ReceivedAt)
        .Take(count)
        .Select(item => new { item.Name, item.Value, item.Unit, item.ReceivedAt })
        .ToListAsync(cancellationToken);
    return Results.Ok(readings);
});

using (var scope = app.Services.CreateScope())
{
    var database = scope.ServiceProvider.GetRequiredService<CommanderDbContext>();
    await database.Database.EnsureCreatedAsync();
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

static bool IsValidTerminalId(string terminalId) =>
    terminalId.Length == 12 && terminalId.All(Uri.IsHexDigit);

static bool IsValidConnection(ConnectionIngestRequest request) =>
    request.Type is "wifi" or "mqtt" &&
    request.State is "connected" or "disconnected" &&
    request.Details is not null &&
    request.Details.Length <= 160;
