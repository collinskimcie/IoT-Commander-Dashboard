namespace IoTCommander.Api;

public sealed class FirmwareImageRecord
{
    public long Id { get; set; }
    public string Version { get; set; } = string.Empty;
    public int Size { get; set; }
    public string Sha256 { get; set; } = string.Empty;
    public DateTimeOffset UploadedAt { get; set; }
    public byte[] Data { get; set; } = [];
}

public sealed record FirmwareRequest(long? ImageId);
public sealed record FirmwareResultRequest(long ImageId, bool Ok, string? Detail = null);
