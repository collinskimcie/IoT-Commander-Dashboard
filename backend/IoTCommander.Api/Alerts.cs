using System.Globalization;

namespace IoTCommander.Api;

public sealed class AlertRuleRecord
{
    public long Id { get; set; }
    public string? TerminalId { get; set; }   // null = every terminal
    public string Metric { get; set; } = string.Empty;
    public string Comparison { get; set; } = ">";
    public double Threshold { get; set; }
    public bool Enabled { get; set; } = true;
    public DateTimeOffset CreatedAt { get; set; }
}

public sealed class AlertEventRecord
{
    public long Id { get; set; }
    public string TerminalId { get; set; } = string.Empty;
    public long RuleId { get; set; }
    public string Metric { get; set; } = string.Empty;
    public double Value { get; set; }
    public string Message { get; set; } = string.Empty;
    public DateTimeOffset OccurredAt { get; set; }
}

public sealed record AlertRuleRequest(
    string? TerminalId, string Metric, string Comparison, double Threshold, bool? Enabled = null);

public static class AlertLogic
{
    public const int CooldownMinutes = 10;   // one alert per rule and terminal in this window
    public const int FreshMinutes = 5;       // older readings are replays and never alert
    public const int MaxRules = 50;

    public static bool ConditionMet(string comparison, double value, double threshold) =>
        comparison switch
        {
            ">" => value > threshold,
            "<" => value < threshold,
            _ => false
        };

    public static string? Validate(AlertRuleRequest? request)
    {
        if (request is null)
        {
            return "A rule is required.";
        }

        var metric = request.Metric?.Trim() ?? string.Empty;
        if (metric.Length < 1 || metric.Length > 24)
        {
            return "Metric must be 1 to 24 characters.";
        }

        if (request.Comparison is not (">" or "<"))
        {
            return "Comparison must be > or <.";
        }

        if (!double.IsFinite(request.Threshold))
        {
            return "Threshold must be a finite number.";
        }

        var terminal = request.TerminalId?.Trim();
        if (!string.IsNullOrEmpty(terminal) && !(terminal.Length == 12 && terminal.All(Uri.IsHexDigit)))
        {
            return "Terminal ID must be 12 hexadecimal characters.";
        }

        return null;
    }

    public static string? NormalizeTerminal(string? terminalId) =>
        string.IsNullOrWhiteSpace(terminalId) ? null : terminalId.Trim().ToUpperInvariant();

    public static string Describe(string metric, string comparison, double threshold, double value, string unit)
    {
        var suffix = string.IsNullOrWhiteSpace(unit) ? string.Empty : " " + unit;
        return string.Create(CultureInfo.InvariantCulture,
            $"{metric} is {value:0.##}{suffix} (limit {comparison} {threshold:0.##})");
    }
}
