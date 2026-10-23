namespace IoTCommander.Api.Tests;

public class AlertLogicTests
{
    private static AlertRuleRequest Rule(
        string? terminal = null, string metric = "temp", string comparison = ">", double threshold = 30) =>
        new(terminal, metric, comparison, threshold);

    [Theory]
    [InlineData(">", 31.0, 30.0, true)]
    [InlineData(">", 30.0, 30.0, false)]
    [InlineData(">", 29.0, 30.0, false)]
    [InlineData("<", 29.0, 30.0, true)]
    [InlineData("<", 30.0, 30.0, false)]
    [InlineData("<", 31.0, 30.0, false)]
    [InlineData("=", 30.0, 30.0, false)]
    [InlineData(">=", 31.0, 30.0, false)]
    public void ConditionMet_FollowsTheComparison(string comparison, double value, double threshold, bool expected) =>
        Assert.Equal(expected, AlertLogic.ConditionMet(comparison, value, threshold));

    [Fact]
    public void Validate_AcceptsAValidRule() => Assert.Null(AlertLogic.Validate(Rule()));

    [Fact]
    public void Validate_RejectsAMissingRule() => Assert.NotNull(AlertLogic.Validate(null));

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("1234567890123456789012345")]
    public void Validate_RejectsBadMetricNames(string metric) =>
        Assert.NotNull(AlertLogic.Validate(Rule(metric: metric)));

    [Fact]
    public void Validate_AcceptsA24CharacterMetric() =>
        Assert.Null(AlertLogic.Validate(Rule(metric: new string('m', 24))));

    [Theory]
    [InlineData("=")]
    [InlineData(">=")]
    [InlineData("")]
    public void Validate_RejectsUnknownComparisons(string comparison) =>
        Assert.NotNull(AlertLogic.Validate(Rule(comparison: comparison)));

    [Theory]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    [InlineData(double.NegativeInfinity)]
    public void Validate_RejectsNonFiniteThresholds(double threshold) =>
        Assert.NotNull(AlertLogic.Validate(Rule(threshold: threshold)));

    [Theory]
    [InlineData(null, true)]
    [InlineData("   ", true)]
    [InlineData("AABBCCDDEEFF", true)]
    [InlineData("aabbccddeeff", true)]
    [InlineData(" AABBCCDDEEFF ", true)]
    [InlineData("AABBCCDDEEF", false)]
    [InlineData("AABBCCDDEEFFA", false)]
    [InlineData("AABBCCDDEEFG", false)]
    public void Validate_ChecksTheTerminalId(string? terminal, bool valid) =>
        Assert.Equal(valid, AlertLogic.Validate(Rule(terminal)) is null);

    [Theory]
    [InlineData(null, null)]
    [InlineData("   ", null)]
    [InlineData("  aabbccddeeff ", "AABBCCDDEEFF")]
    public void NormalizeTerminal_TrimsAndUppercases(string? input, string? expected) =>
        Assert.Equal(expected, AlertLogic.NormalizeTerminal(input));

    [Fact]
    public void Describe_RoundsAndIncludesTheUnit() =>
        Assert.Equal("temp is 31.46 C (limit > 30)", AlertLogic.Describe("temp", ">", 30, 31.456, "C"));

    [Fact]
    public void Describe_OmitsAMissingUnit() =>
        Assert.Equal("temp is 5 (limit < 10)", AlertLogic.Describe("temp", "<", 10, 5, "  "));
}

public class AlertDecisionTests
{
    private static readonly DateTimeOffset Received = new(2026, 10, 23, 12, 0, 0, TimeSpan.Zero);

    private static LatestReading Before(double value, DateTimeOffset? at = null) =>
        new("temp", value, "C", at ?? Received.AddMinutes(-1));

    [Fact]
    public void ShouldFire_WhenTheLimitIsCrossed() =>
        Assert.True(AlertDecision.ShouldFire(">", 30, 31, Before(25), false));

    [Fact]
    public void ShouldFire_WhenThereIsNoPreviousReading() =>
        Assert.True(AlertDecision.ShouldFire(">", 30, 31, null, false));

    [Fact]
    public void ShouldFire_ForALessThanRule() =>
        Assert.True(AlertDecision.ShouldFire("<", 10, 5, Before(15), false));

    [Fact]
    public void ShouldNotFire_WhenAlreadyOverTheLimit() =>
        Assert.False(AlertDecision.ShouldFire(">", 30, 35, Before(33), false));

    [Fact]
    public void ShouldNotFire_WhileCoolingDown() =>
        Assert.False(AlertDecision.ShouldFire(">", 30, 31, Before(25), true));

    [Fact]
    public void ShouldNotFire_WhenTheConditionIsNotMet() =>
        Assert.False(AlertDecision.ShouldFire(">", 30, 29, Before(25), false));

    [Fact]
    public void IsFresh_AcceptsExactlyFiveMinutesOld() =>
        Assert.True(AlertDecision.IsFresh(Received.AddMinutes(-5), Received));

    [Fact]
    public void IsFresh_RejectsOlderThanFiveMinutes() =>
        Assert.False(AlertDecision.IsFresh(Received.AddMinutes(-5).AddSeconds(-1), Received));

    [Fact]
    public void IsOlderThanStored_IsFalseWithoutAStoredValue() =>
        Assert.False(AlertDecision.IsOlderThanStored(null, Received));

    [Fact]
    public void IsOlderThanStored_ComparesTimestamps()
    {
        var stored = Before(20, Received);
        Assert.True(AlertDecision.IsOlderThanStored(stored, Received.AddSeconds(-1)));
        Assert.False(AlertDecision.IsOlderThanStored(stored, Received));
        Assert.False(AlertDecision.IsOlderThanStored(stored, Received.AddSeconds(1)));
    }
}
