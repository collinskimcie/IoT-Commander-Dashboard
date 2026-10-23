namespace IoTCommander.Api.Tests;

public class TelemetryLogicTests
{
    private static readonly DateTimeOffset Received = new(2026, 10, 23, 12, 0, 0, TimeSpan.Zero);

    private static (ReadingIngestRequest Reading, DateTimeOffset At) R(
        string name, double value, DateTimeOffset at, string? unit = "C") =>
        (new ReadingIngestRequest(name, value, unit), at);

    // ---- ResolveTime (observedAt handling) ----

    [Fact]
    public void ResolveTime_WithoutObservedAt_UsesTheReceivedTime()
    {
        var (at, observed) = TelemetryLogic.ResolveTime(null, Received);
        Assert.Equal(Received, at);
        Assert.False(observed);
    }

    [Theory]
    [InlineData(0L)]
    [InlineData(-5L)]
    [InlineData(253402300799L)]
    [InlineData(long.MaxValue)]
    public void ResolveTime_RejectsImplausibleSeconds(long seconds)
    {
        var (at, observed) = TelemetryLogic.ResolveTime(seconds, Received);
        Assert.Equal(Received, at);
        Assert.False(observed);
    }

    [Fact]
    public void ResolveTime_TrustsARecentObservation()
    {
        var seen = Received.AddHours(-1);
        var (at, observed) = TelemetryLogic.ResolveTime(seen.ToUnixTimeSeconds(), Received);
        Assert.Equal(seen, at);
        Assert.True(observed);
    }

    [Fact]
    public void ResolveTime_AcceptsExactlySevenDaysOld()
    {
        var seen = Received.AddDays(-7);
        var (at, observed) = TelemetryLogic.ResolveTime(seen.ToUnixTimeSeconds(), Received);
        Assert.Equal(seen, at);
        Assert.True(observed);
    }

    [Fact]
    public void ResolveTime_RejectsOlderThanSevenDays()
    {
        var seen = Received.AddDays(-7).AddSeconds(-1);
        var (at, observed) = TelemetryLogic.ResolveTime(seen.ToUnixTimeSeconds(), Received);
        Assert.Equal(Received, at);
        Assert.False(observed);
    }

    [Theory]
    [InlineData(3 * 60)]
    [InlineData(5 * 60)]
    public void ResolveTime_ClampsSmallClockSkewToTheReceivedTime(int secondsAhead)
    {
        var seen = Received.AddSeconds(secondsAhead);
        var (at, observed) = TelemetryLogic.ResolveTime(seen.ToUnixTimeSeconds(), Received);
        Assert.Equal(Received, at);
        Assert.True(observed);
    }

    [Fact]
    public void ResolveTime_RejectsTimestampsMoreThanFiveMinutesAhead()
    {
        var seen = Received.AddMinutes(5).AddSeconds(1);
        var (at, observed) = TelemetryLogic.ResolveTime(seen.ToUnixTimeSeconds(), Received);
        Assert.Equal(Received, at);
        Assert.False(observed);
    }

    // ---- ParseLatest ----

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("not json")]
    public void ParseLatest_TreatsMissingOrCorruptJsonAsEmpty(string? json) =>
        Assert.Empty(TelemetryLogic.ParseLatest(json));

    // ---- MergeLatest ----

    [Fact]
    public void MergeLatest_StoresReadingsSortedByName()
    {
        var json = TelemetryLogic.MergeLatest(null, new[] { R("temp", 21.5, Received), R("hum", 40, Received) });
        var latest = TelemetryLogic.ParseLatest(json);
        Assert.Equal(new[] { "hum", "temp" }, latest.Select(item => item.Name).ToArray());
    }

    [Fact]
    public void MergeLatest_ANewerReadingReplacesTheStoredValue()
    {
        var first = TelemetryLogic.MergeLatest(null, new[] { R("temp", 20, Received) });
        var second = TelemetryLogic.MergeLatest(first, new[] { R("temp", 25, Received.AddMinutes(1)) });
        var temp = Assert.Single(TelemetryLogic.ParseLatest(second));
        Assert.Equal(25, temp.Value);
        Assert.Equal(Received.AddMinutes(1), temp.At);
    }

    [Fact]
    public void MergeLatest_AReplayedOlderReadingDoesNotReplaceAFresherValue()
    {
        var first = TelemetryLogic.MergeLatest(null, new[] { R("temp", 25, Received) });
        var second = TelemetryLogic.MergeLatest(first, new[] { R("temp", 10, Received.AddMinutes(-30)) });
        var temp = Assert.Single(TelemetryLogic.ParseLatest(second));
        Assert.Equal(25, temp.Value);
        Assert.Equal(Received, temp.At);
    }

    [Fact]
    public void MergeLatest_AReadingWithTheSameTimestampReplacesTheValue()
    {
        var first = TelemetryLogic.MergeLatest(null, new[] { R("temp", 20, Received) });
        var second = TelemetryLogic.MergeLatest(first, new[] { R("temp", 22, Received) });
        Assert.Equal(22, Assert.Single(TelemetryLogic.ParseLatest(second)).Value);
    }

    [Fact]
    public void MergeLatest_KeepsMetricsThatWereNotUpdated()
    {
        var first = TelemetryLogic.MergeLatest(null, new[] { R("temp", 20, Received), R("hum", 40, Received) });
        var second = TelemetryLogic.MergeLatest(first, new[] { R("temp", 21, Received.AddMinutes(1)) });
        var latest = TelemetryLogic.ParseLatest(second).ToDictionary(item => item.Name);
        Assert.Equal(40, latest["hum"].Value);
        Assert.Equal(21, latest["temp"].Value);
    }

    [Fact]
    public void MergeLatest_TreatsCorruptStoredJsonAsEmpty()
    {
        var json = TelemetryLogic.MergeLatest("not json", new[] { R("temp", 20, Received) });
        Assert.Single(TelemetryLogic.ParseLatest(json));
    }

    [Fact]
    public void MergeLatest_StoresAMissingUnitAsAnEmptyString()
    {
        var json = TelemetryLogic.MergeLatest(null, new[] { R("temp", 20, Received, unit: null) });
        Assert.Equal(string.Empty, Assert.Single(TelemetryLogic.ParseLatest(json)).Unit);
    }

    [Fact]
    public void MergeLatest_NeverStoresMoreThanEightMetrics()
    {
        var many = Enumerable.Range(0, 10).Select(index => R($"m{index:00}", index, Received)).ToArray();
        Assert.Equal(8, TelemetryLogic.ParseLatest(TelemetryLogic.MergeLatest(null, many)).Count);
    }
}
