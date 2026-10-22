using Microsoft.EntityFrameworkCore;

namespace IoTCommander.Api;

public sealed class CommanderDbContext(DbContextOptions<CommanderDbContext> options) : DbContext(options)
{
    public DbSet<TerminalRecord> Terminals => Set<TerminalRecord>();
    public DbSet<ConnectionEventRecord> ConnectionEvents => Set<ConnectionEventRecord>();
    public DbSet<TelemetryRecord> TelemetryRecords => Set<TelemetryRecord>();
    public DbSet<HeartbeatRecord> HeartbeatRecords => Set<HeartbeatRecord>();

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<TerminalRecord>(entity =>
        {
            entity.Property(item => item.Name).HasMaxLength(40);
            entity.Property(item => item.GroupName).HasMaxLength(40);
            entity.Property(item => item.Firmware).HasMaxLength(24);
            entity.Property(item => item.ResetReason).HasMaxLength(16);
        });
        modelBuilder.Entity<HeartbeatRecord>(entity =>
        {
            entity.HasKey(item => item.Id);
            entity.HasIndex(item => new { item.TerminalId, item.ReceivedAt });
            entity.HasOne<TerminalRecord>().WithMany().HasForeignKey(item => item.TerminalId);
        });
        modelBuilder.Entity<TerminalRecord>(entity =>
        {
            entity.HasKey(item => item.Id);
            entity.Property(item => item.Id).HasMaxLength(12);
        });
        modelBuilder.Entity<ConnectionEventRecord>(entity =>
        {
            entity.HasKey(item => item.Id);
            entity.Property(item => item.Type).HasMaxLength(8);
            entity.Property(item => item.State).HasMaxLength(12);
            entity.Property(item => item.Details).HasMaxLength(160);
            entity.HasIndex(item => new { item.TerminalId, item.OccurredAt });
            entity.HasOne<TerminalRecord>().WithMany().HasForeignKey(item => item.TerminalId);
        });
        modelBuilder.Entity<TelemetryRecord>(entity =>
        {
            entity.HasKey(item => item.Id);
            entity.Property(item => item.Name).HasMaxLength(24);
            entity.Property(item => item.Unit).HasMaxLength(8);
            entity.HasIndex(item => new { item.TerminalId, item.ReceivedAt });
            entity.HasOne<TerminalRecord>().WithMany().HasForeignKey(item => item.TerminalId);
        });
    }
}

public sealed class TerminalRecord
{
    public string Id { get; set; } = string.Empty;
    public DateTimeOffset FirstSeenAt { get; set; }
    public DateTimeOffset LastSeenAt { get; set; }
    public string? Name { get; set; }
    public string? GroupName { get; set; }
    public string? Firmware { get; set; }
    public long? UptimeSeconds { get; set; }
    public long? HeapFree { get; set; }
    public long? HeapMaxBlock { get; set; }
    public long? PsramFree { get; set; }
    public int? Rssi { get; set; }
    public string? ResetReason { get; set; }
    public DateTimeOffset? LastBootAt { get; set; }
    public int BootCount { get; set; }
    public string? LatestReadings { get; set; }
}

public sealed class ConnectionEventRecord
{
    public long Id { get; set; }
    public string TerminalId { get; set; } = string.Empty;
    public string Type { get; set; } = string.Empty;
    public string State { get; set; } = string.Empty;
    public string Details { get; set; } = string.Empty;
    public DateTimeOffset OccurredAt { get; set; }
}

public sealed class TelemetryRecord
{
    public long Id { get; set; }
    public string TerminalId { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
    public double Value { get; set; }
    public string Unit { get; set; } = string.Empty;
    public DateTimeOffset ReceivedAt { get; set; }
}

public sealed record ConnectionIngestRequest(string Type, string State, string Details);
public sealed record TelemetryIngestRequest(IReadOnlyList<ReadingIngestRequest> Readings);
public sealed record ReadingIngestRequest(string Name, double Value, string? Unit = null);

public sealed class HeartbeatRecord
{
    public long Id { get; set; }
    public string TerminalId { get; set; } = string.Empty;
    public DateTimeOffset ReceivedAt { get; set; }
    public int? Rssi { get; set; }
    public long? HeapFree { get; set; }
}

public sealed record HeartbeatIngestRequest(
    string? Fw = null,
    long? UptimeSeconds = null,
    long? HeapFree = null,
    long? HeapMaxBlock = null,
    long? PsramFree = null,
    int? Rssi = null,
    string? ResetReason = null);

public sealed record TerminalUpdateRequest(string? Name, string? GroupName);
public sealed record LatestReading(string Name, double Value, string Unit, DateTimeOffset At);
