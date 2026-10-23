import { useCallback, useEffect, useState } from 'react';
import './alerts.css';

const POLL_MS = 15000;

async function call(path, options = {}) {
  const response = await fetch(path, {
    headers: { Accept: 'application/json', 'Content-Type': 'application/json' },
    ...options
  });
  if (!response.ok) {
    const detail = await response.text().catch(() => '');
    throw new Error(`HTTP ${response.status}${detail ? ` - ${detail.slice(0, 140)}` : ''}`);
  }
  return response.status === 204 ? null : response.json();
}

function when(value) {
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? ''
    : date.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
}

export default function AlertsPanel({ devices = [] }) {
  const [rules, setRules] = useState([]);
  const [events, setEvents] = useState([]);
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);
  const [form, setForm] = useState({ terminalId: '', metric: '', comparison: '>', threshold: '' });

  const metricNames = [
    ...new Set(devices.flatMap((device) => (device.latest || []).map((reading) => reading.name)))
  ];
  const deviceLabel = (id) => {
    if (!id) return 'All terminals';
    const device = devices.find((item) => item.id === id);
    return device?.name || id;
  };

  const load = useCallback(async () => {
    try {
      const [ruleList, eventList] = await Promise.all([
        call('/api/v1/alert-rules'),
        call('/api/v1/alerts?limit=15')
      ]);
      setRules(Array.isArray(ruleList) ? ruleList : []);
      setEvents(Array.isArray(eventList) ? eventList : []);
      setError('');
    } catch (err) {
      setError(err.message || 'Could not load alerts.');
    }
  }, []);

  useEffect(() => {
    load();
    const refresh = () => {
      if (document.visibilityState !== 'hidden') load();
    };
    const timer = window.setInterval(refresh, POLL_MS);
    document.addEventListener('visibilitychange', refresh);
    return () => {
      window.clearInterval(timer);
      document.removeEventListener('visibilitychange', refresh);
    };
  }, [load]);

  const setField = (key, value) => setForm((current) => ({ ...current, [key]: value }));

  async function addRule(event) {
    event.preventDefault();
    const threshold = Number(form.threshold);
    if (!form.metric.trim() || form.threshold === '' || !Number.isFinite(threshold)) {
      setError('Enter a metric name and a numeric threshold.');
      return;
    }
    setSaving(true);
    try {
      await call('/api/v1/alert-rules', {
        method: 'POST',
        body: JSON.stringify({
          terminalId: form.terminalId || null,
          metric: form.metric.trim(),
          comparison: form.comparison,
          threshold
        })
      });
      setForm((current) => ({ ...current, metric: '', threshold: '' }));
      await load();
    } catch (err) {
      setError(err.message || 'Could not save the rule.');
    } finally {
      setSaving(false);
    }
  }

  async function toggleRule(rule) {
    try {
      await call(`/api/v1/alert-rules/${rule.id}`, {
        method: 'PUT',
        body: JSON.stringify({
          terminalId: rule.terminalId,
          metric: rule.metric,
          comparison: rule.comparison,
          threshold: rule.threshold,
          enabled: !rule.enabled
        })
      });
      await load();
    } catch (err) {
      setError(err.message || 'Could not update the rule.');
    }
  }

  async function removeRule(rule) {
    try {
      await call(`/api/v1/alert-rules/${rule.id}`, { method: 'DELETE' });
      await load();
    } catch (err) {
      setError(err.message || 'Could not delete the rule.');
    }
  }

  return (
    <section className="panel alerts-panel" id="alerts">
      <div className="panel-heading">
        <div>
          <h2>Alert rules</h2>
          <p>
            An alert fires when a reading crosses its limit, re-arms when the value returns to
            normal, and pauses for 10 minutes per rule.
          </p>
        </div>
        <span className="count-badge">{rules.length} rules</span>
      </div>

      {error && <div className="alerts-error">{error}</div>}

      <form className="alert-form" onSubmit={addRule}>
        <label>
          <span>Terminal</span>
          <select value={form.terminalId} onChange={(e) => setField('terminalId', e.target.value)}>
            <option value="">All terminals</option>
            {devices.map((device) => (
              <option key={device.id} value={device.id}>{device.name || device.id}</option>
            ))}
          </select>
        </label>
        <label>
          <span>Metric</span>
          <input
            list="alert-metrics"
            value={form.metric}
            maxLength={24}
            placeholder="temperature"
            onChange={(e) => setField('metric', e.target.value)}
          />
          <datalist id="alert-metrics">
            {metricNames.map((name) => <option key={name} value={name} />)}
          </datalist>
        </label>
        <label>
          <span>When</span>
          <select value={form.comparison} onChange={(e) => setField('comparison', e.target.value)}>
            <option value=">">goes above</option>
            <option value="<">drops below</option>
          </select>
        </label>
        <label>
          <span>Limit</span>
          <input
            type="number"
            step="any"
            value={form.threshold}
            onChange={(e) => setField('threshold', e.target.value)}
          />
        </label>
        <button type="submit" disabled={saving}>{saving ? 'Saving...' : 'Add rule'}</button>
      </form>

      {rules.length === 0 ? (
        <div className="empty-state compact">No alert rules yet.</div>
      ) : (
        <ul className="alert-list">
          {rules.map((rule) => (
            <li key={rule.id} className={rule.enabled ? '' : 'alert-off'}>
              <span>
                <strong>{rule.metric}</strong> {rule.comparison === '>' ? 'above' : 'below'}{' '}
                <strong>{rule.threshold}</strong>
                <small> - {deviceLabel(rule.terminalId)}</small>
              </span>
              <span className="alert-actions">
                <button type="button" onClick={() => toggleRule(rule)}>
                  {rule.enabled ? 'Disable' : 'Enable'}
                </button>
                <button type="button" onClick={() => removeRule(rule)}>Delete</button>
              </span>
            </li>
          ))}
        </ul>
      )}

      <h3 className="alert-subhead">Recent alerts</h3>
      {events.length === 0 ? (
        <div className="empty-state compact">No alerts have fired.</div>
      ) : (
        <ul className="alert-list">
          {events.map((item) => (
            <li key={item.id}>
              <span>
                {item.message}
                <small> - {deviceLabel(item.terminalId)}</small>
              </span>
              <small>{when(item.occurredAt)}</small>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
