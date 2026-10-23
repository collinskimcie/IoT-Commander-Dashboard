namespace IoTCommander.Api;

// The fire/skip decision from RaiseAlerts, without the database. Behaviour is unchanged.
public static class AlertDecision
{
    public static bool IsOlderThanStored(LatestReading? before, DateTimeOffset at) =>
        before is not null && before.At > at;

    public static bool IsFresh(DateTimeOffset at, DateTimeOffset receivedAt) =>
        at >= receivedAt.AddMinutes(-AlertLogic.FreshMinutes);

    // Fires only on a crossing: condition met now, not met by the previous value, and not cooling down.
    public static bool ShouldFire(
        string comparison, double threshold, double value, LatestReading? before, bool coolingDown)
    {
        var nowMet = AlertLogic.ConditionMet(comparison, value, threshold);
        var wasMet = before is not null && AlertLogic.ConditionMet(comparison, before.Value, threshold);
        return nowMet && !wasMet && !coolingDown;
    }
}
