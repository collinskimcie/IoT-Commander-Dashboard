namespace IoTCommander.Api;

// Pure logic extracted from Program.cs so it can be unit tested. Behaviour is unchanged.
public static class TelemetryLogic
{
    // Store-and-forward: a reading may carry the time it was observed. Trust it only when plausible.
    public static (DateTimeOffset At, bool Observed) ResolveTime(long? observedAt, DateTimeOffset receivedAt)
    {
        if (observedAt is { } seconds && seconds > 0 && seconds < 253402300799)
        {
            var observed = DateTimeOffset.FromUnixTimeSeconds(seconds);
            if (observed >= receivedAt.AddDays(-7) && observed <= receivedAt.AddMinutes(5))
            {
                return (observed > receivedAt ? receivedAt : observed, true);
            }
        }
        return (receivedAt, false);
    }

    public static List<LatestReading> ParseLatest(string? json)
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

    public static string MergeLatest(string? existing, IEnumerable<(ReadingIngestRequest Reading, DateTimeOffset At)> incoming)
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
}
