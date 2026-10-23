import { useCallback, useEffect, useMemo, useState } from 'react';
import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis
} from 'recharts';
import { FleetOverview, HealthPanel } from './Fleet';
import AlertsPanel from './Alerts';

const REFRESH_MS = 15000;
const isTabVisible = () =>
  typeof document === 'undefined' || document.visibilityState !== 'hidden';
const RANGES = [
  { label: '1 hour', value: '1h', ms: 60 * 60 * 1000 },
  { label: '24 hours', value: '24h', ms: 24 * 60 * 60 * 1000 },
  { label: '7 days', value: '7d', ms: 7 * 24 * 60 * 60 * 1000 },
  { label: '30 days', value: '30d', ms: 30 * 24 * 60 * 60 * 1000 }
];

async function api(path, signal) {
  const response = await fetch(path, {
    headers: { Accept: 'application/json' },
    signal
  });

  if (!response.ok) {
    const detail = await response.text().catch(() => '');
    throw new Error(
      `API returned HTTP ${response.status}${detail ? ` — ${detail.slice(0, 160)}` : ''}`
    );
  }

  return response.json();
}

function Sparkline({ values }) {
  if (!Array.isArray(values) || values.length < 2) return null;
  const width = 120;
  const height = 24;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const flat = max === min;
  const step = width / (values.length - 1);
  const path = values
    .map((value, index) => {
      const x = (index * step).toFixed(1);
      const y = flat
        ? height / 2
        : (height - 2 - ((value - min) / (max - min)) * (height - 4)).toFixed(1);
      return `${index === 0 ? 'M' : 'L'}${x} ${y}`;
    })
    .join(' ');
  return (
    <svg
      className="sparkline"
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      role="img"
      aria-label="Last hour trend"
    >
      <path
        d={path}
        fill="none"
        stroke="#12a980"
        strokeWidth="1.6"
        strokeLinejoin="round"
        strokeLinecap="round"
        vectorEffect="non-scaling-stroke"
      />
    </svg>
  );
}

function deviceStatus(device) {
  if (device.status) return device.status;

  // Compatibility fallback for an API response without the computed status field.
  if (!device.lastSeenAt) return 'never_seen';

  const ageSeconds = Math.max(
    0,
    (Date.now() - new Date(device.lastSeenAt).getTime()) / 1000
  );

  if (!Number.isFinite(ageSeconds)) return 'never_seen';
  if (ageSeconds <= 60) return 'online';
  if (ageSeconds <= 180) return 'stale';
  return 'offline';
}

function formatDate(value) {
  if (!value) return 'Never';
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? 'Unknown'
    : date.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
}

function formatTime(value) {
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? ''
    : date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function telemetryFreshness(item, currentTime = Date.now()) {
  if (!item || !item.receivedAt) {
    return { status: 'missing', label: 'No data', age: 'No samples' };
  }

  const timestamp = new Date(item.receivedAt).getTime();
  if (!Number.isFinite(timestamp)) {
    return { status: 'missing', label: 'No data', age: 'Unknown age' };
  }

  const seconds = Math.max(0, Math.floor((currentTime - timestamp) / 1000));
  const age = seconds < 60
    ? `${seconds}s ago`
    : seconds < 3600
      ? `${Math.floor(seconds / 60)}m ago`
      : seconds < 86400
        ? `${Math.floor(seconds / 3600)}h ago`
        : `${Math.floor(seconds / 86400)}d ago`;

  return seconds <= 120
    ? { status: 'fresh', label: 'Fresh', age }
    : { status: 'stale', label: 'Stale', age };
}

function ageLabel(device) {
  if (!device.lastSeenAt) return 'No heartbeat received';

  const seconds = Math.max(
    0,
    Math.floor((Date.now() - new Date(device.lastSeenAt).getTime()) / 1000)
  );

  if (!Number.isFinite(seconds)) return 'Unknown last-seen time';
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
  return `${Math.floor(seconds / 86400)}d ago`;
}

function StatusPill({ status }) {
  const labels = {
    online: 'Online',
    stale: 'Stale',
    offline: 'Offline',
    never_seen: 'Never seen'
  };

  return (
    <span className={`status-pill status-${status}`}>
      <span className="status-dot" />
      {labels[status] || status}
    </span>
  );
}

function MetricIcon() {
  return <span className="metric-icon" aria-hidden="true">⌁</span>;
}

export default function App() {
  const [devices, setDevices] = useState([]);
  const [selectedId, setSelectedId] = useState('');
  const [telemetry, setTelemetry] = useState([]);
  const [latestTelemetry, setLatestTelemetry] = useState([]);
  const [connections, setConnections] = useState([]);
  const [selectedMetric, setSelectedMetric] = useState('');
  const [range, setRange] = useState('24h');
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState('');
  const [lastUpdated, setLastUpdated] = useState(null);
  const [now, setNow] = useState(Date.now());
  const [sparklines, setSparklines] = useState({});

  const selectedDevice = devices.find((device) => device.id === selectedId);
  const selectedRange = RANGES.find((item) => item.value === range) || RANGES[1];

  const loadData = useCallback(async (signal, isInitial = false) => {
    if (isInitial) setLoading(true);
    else setRefreshing(true);

    try {
      const deviceList = await api('/api/v1/terminals', signal);
      if (!Array.isArray(deviceList)) {
        throw new Error('The terminals endpoint did not return a list.');
      }

      setDevices(deviceList);
      // Optional extra: an older backend without this route simply shows no sparklines.
      api('/api/v1/terminals/sparklines?minutes=60&points=30', signal)
        .then((data) => {
          if (data && typeof data === 'object' && !Array.isArray(data)) setSparklines(data);
        })
        .catch(() => {});
      setSelectedId((current) => {
        if (current && deviceList.some((device) => device.id === current)) {
          return current;
        }
        return deviceList[0]?.id || '';
      });
      setError('');
      setLastUpdated(new Date());
    } catch (err) {
      if (err.name !== 'AbortError') {
        setError(err.message || 'Could not load devices.');
      }
    } finally {
      if (!signal?.aborted) {
        setLoading(false);
        setRefreshing(false);
      }
    }
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    loadData(controller.signal, true);

    const refreshNow = () => {
      if (!isTabVisible()) return;
      loadData(undefined, false);
      setNow(Date.now());
    };
    const interval = window.setInterval(refreshNow, REFRESH_MS);
    document.addEventListener('visibilitychange', refreshNow);

    return () => {
      controller.abort();
      window.clearInterval(interval);
      document.removeEventListener('visibilitychange', refreshNow);
    };
  }, [loadData]);

  useEffect(() => {
    if (!selectedId) {
      setTelemetry([]);
      setLatestTelemetry([]);
      setConnections([]);
      setSelectedMetric('');
      return undefined;
    }

    const controller = new AbortController();
    let active = true;

    async function loadSelectedDevice() {
      const to = new Date();
      const from = new Date(to.getTime() - selectedRange.ms);
      const params = new URLSearchParams({
        limit: '500',
        from: from.toISOString(),
        to: to.toISOString(),
        ascending: 'true'
      });

      const latestParams = new URLSearchParams({
        limit: '500',
        ascending: 'true'
      });

      const results = await Promise.allSettled([
        api(
          `/api/v1/terminals/${encodeURIComponent(selectedId)}/telemetry?${params}`,
          controller.signal
        ),
        api(
          `/api/v1/terminals/${encodeURIComponent(selectedId)}/telemetry?${latestParams}`,
          controller.signal
        ),
        api(
          `/api/v1/terminals/${encodeURIComponent(selectedId)}/connections?limit=30`,
          controller.signal
        )
      ]);

      if (!active) return;

      const telemetryResult = results[0];
      const latestTelemetryResult = results[1];
      const connectionResult = results[2];

      const latestSorted =
        latestTelemetryResult.status === 'fulfilled' &&
        Array.isArray(latestTelemetryResult.value)
          ? [...latestTelemetryResult.value].sort(
              (a, b) => new Date(a.receivedAt) - new Date(b.receivedAt)
            )
          : [];

      if (telemetryResult.status === 'fulfilled' &&
          Array.isArray(telemetryResult.value)) {
        const sorted = [...telemetryResult.value].sort(
          (a, b) => new Date(a.receivedAt) - new Date(b.receivedAt)
        );
        setTelemetry(sorted);
        setSelectedMetric((current) => {
          const availableNames = new Set([
            ...sorted.map((item) => item.name),
            ...latestSorted.map((item) => item.name)
          ]);
          if (current && availableNames.has(current)) return current;
          return latestSorted.at(-1)?.name ||
            sorted.at(-1)?.name ||
            sorted[0]?.name ||
            '';
        });
      } else if (telemetryResult.status === 'rejected' &&
                 telemetryResult.reason?.name !== 'AbortError') {
        setError(`Telemetry: ${telemetryResult.reason?.message || 'request failed'}`);
      }

      if (latestTelemetryResult.status === 'fulfilled' &&
          Array.isArray(latestTelemetryResult.value)) {
        setLatestTelemetry(latestSorted);
      } else if (latestTelemetryResult.status === 'rejected' &&
                 latestTelemetryResult.reason?.name !== 'AbortError') {
        setLatestTelemetry([]);
        setError(
          `Latest telemetry: ${latestTelemetryResult.reason?.message || 'request failed'}`
        );
      }

      if (connectionResult.status === 'fulfilled' &&
          Array.isArray(connectionResult.value)) {
        setConnections(connectionResult.value);
      } else if (connectionResult.status === 'rejected' &&
                 connectionResult.reason?.name !== 'AbortError') {
        setConnections([]);
      }
    }

    loadSelectedDevice();

    const refreshSelected = () => {
      if (isTabVisible()) loadSelectedDevice();
    };
    const interval = window.setInterval(refreshSelected, REFRESH_MS);
    document.addEventListener('visibilitychange', refreshSelected);
    return () => {
      active = false;
      controller.abort();
      window.clearInterval(interval);
      document.removeEventListener('visibilitychange', refreshSelected);
    };
  }, [selectedId, selectedRange.ms]);

  const metricNames = useMemo(
    () => [...new Set(
      [...telemetry, ...latestTelemetry].map((item) => item.name).filter(Boolean)
    )],
    [telemetry, latestTelemetry]
  );

  const chartData = useMemo(
    () => telemetry
      .filter((item) => item.name === selectedMetric)
      .map((item) => ({
        timestamp: item.receivedAt,
        time: formatTime(item.receivedAt),
        value: Number(item.value),
        unit: item.unit || ''
      }))
      .filter((item) => Number.isFinite(item.value)),
    [telemetry, selectedMetric]
  );

  const latestReading = latestTelemetry
    .filter((item) => item.name === selectedMetric)
    .at(-1);
  const latestFreshness = telemetryFreshness(latestReading, now);
  const onlineCount = devices.filter((device) => deviceStatus(device) === 'online').length;
  const staleCount = devices.filter((device) => deviceStatus(device) === 'stale').length;
  const offlineCount = devices.filter(
    (device) => ['offline', 'never_seen'].includes(deviceStatus(device))
  ).length;

  const latestByMetric = useMemo(() => {
    const map = new Map();
    for (const item of latestTelemetry) {
      const existing = map.get(item.name);
      if (!existing || new Date(item.receivedAt) > new Date(existing.receivedAt)) {
        map.set(item.name, item);
      }
    }
    return [...map.values()].sort((a, b) => a.name.localeCompare(b.name));
  }, [latestTelemetry]);

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <a className="brand" href="#overview" aria-label="IoT Commander home">
          <span className="brand-mark">IO</span>
          <span className="brand-copy">
            <strong>IoT Commander</strong>
            <small>DEVICE OPERATIONS</small>
          </span>
        </a>

        <div className="side-label">WORKSPACE</div>
        <a className="nav-item active" href="#overview"><span>◫</span> Overview</a>
        <a className="nav-item" href="#devices"><span>▦</span> Devices</a>
        <a className="nav-item" href="#telemetry"><span>⌁</span> Telemetry</a>
        <a className="nav-item" href="#connections"><span>↔</span> Connections</a>

        <div className="sidebar-bottom">
          <span className="live-indicator" />
          <div>
            <strong>Local API</strong>
            <small>127.0.0.1:8080</small>
          </div>
        </div>
      </aside>

      <section className="main-panel" id="overview">
        <header className="topbar">
          <div>
            <div className="eyebrow">OPERATIONS CENTER / OVERVIEW</div>
            <h1>Device monitoring</h1>
            <p className="subheading">A live view of your connected IoT systems.</p>
          </div>
          <div className="topbar-actions">
            <div className="refresh-meta">
              <span className={`live-indicator ${refreshing ? 'pulse' : ''}`} />
              <span>{refreshing ? 'Refreshing…' : 'Auto-refresh · 15s'}</span>
              <small>{lastUpdated ? `Updated ${formatTime(lastUpdated)}` : 'Waiting for API'}</small>
            </div>
            <button className="refresh-button" onClick={() => loadData()} disabled={refreshing}>
              <span aria-hidden="true">↻</span> Refresh
            </button>
          </div>
        </header>

        {error && (
          <div className="error-banner" role="alert">
            <span>!</span>
            <div>
              <strong>Some data could not be loaded</strong>
              <p>{error}. Check that the API is running and the development proxy can read backend/.env.</p>
            </div>
            <button onClick={() => setError('')} aria-label="Dismiss error">×</button>
          </div>
        )}

        <section className="stats-grid" aria-label="Device summary">
          <article className="stat-card">
            <div className="stat-top"><span>Total devices</span><span className="stat-icon">▦</span></div>
            <div className="stat-value">{loading ? '—' : devices.length}</div>
            <div className="stat-foot">Registered terminals</div>
          </article>
          <article className="stat-card">
            <div className="stat-top"><span>Online</span><span className="stat-icon green">↗</span></div>
            <div className="stat-value">{loading ? '—' : onlineCount}</div>
            <div className="stat-foot"><span className="tiny-dot green-bg" /> Reporting recently</div>
          </article>
          <article className="stat-card">
            <div className="stat-top"><span>Stale</span><span className="stat-icon amber">◷</span></div>
            <div className="stat-value">{loading ? '—' : staleCount}</div>
            <div className="stat-foot">Last seen 61–180s ago</div>
          </article>
          <article className="stat-card">
            <div className="stat-top"><span>Offline / unknown</span><span className="stat-icon red">⌁</span></div>
            <div className="stat-value">{loading ? '—' : offlineCount}</div>
            <div className="stat-foot">No recent heartbeat</div>
          </article>
        </section>

        <FleetOverview devices={devices} selectedId={selectedId} onSelect={setSelectedId} />

        <section className="content-grid">
          <article className="panel device-panel" id="devices">
            <div className="panel-heading">
              <div>
                <h2>Registered devices</h2>
                <p>Select a terminal to inspect its telemetry.</p>
              </div>
              <span className="count-badge">{devices.length} total</span>
            </div>

            {loading && devices.length === 0 ? (
              <div className="empty-state"><span className="loader" /> Loading devices…</div>
            ) : devices.length === 0 ? (
              <div className="empty-state">
                <div className="empty-icon">⌁</div>
                <strong>No devices registered yet</strong>
                <p>Once a terminal reports a heartbeat, it will appear here.</p>
              </div>
            ) : (
              <div className="device-list">
                {devices.map((device) => {
                  const status = deviceStatus(device);
                  const selected = device.id === selectedId;
                  return (
                    <button
                      key={device.id}
                      className={`device-row ${selected ? 'selected' : ''}`}
                      onClick={() => setSelectedId(device.id)}
                    >
                      <span className={`device-avatar avatar-${status}`}>⌁</span>
                      <span className="device-info">
                        <strong>{device.name || device.id}</strong>
                        <small>Last seen {ageLabel(device)}</small>
                        <Sparkline values={Object.values(sparklines[device.id] || {})[0]} />
                      </span>
                      <StatusPill status={status} />
                      <span className="row-chevron">›</span>
                    </button>
                  );
                })}
              </div>
            )}
          </article>

          <article className="panel selected-panel">
            <div className="panel-heading">
              <div>
                <h2>Selected terminal</h2>
                <p>Connection and reporting status</p>
              </div>
              <span className="terminal-symbol">⌘</span>
            </div>
            {selectedDevice ? (
              <>
                <div className="selected-id">{selectedDevice.id}</div>
                <StatusPill status={deviceStatus(selectedDevice)} />
                <div className="detail-list">
                  <div><span>First registered</span><strong>{formatDate(selectedDevice.firstSeenAt)}</strong></div>
                  <div><span>Last heartbeat / report</span><strong>{formatDate(selectedDevice.lastSeenAt)}</strong></div>
                  <div><span>Age</span><strong>{ageLabel(selectedDevice)}</strong></div>
                </div>
                <div className="selected-note">
                  Status is inferred from the backend's last-seen timestamp. MQTT connectivity is shown separately in connection events.
                </div>
                <HealthPanel device={selectedDevice} onChanged={() => loadData()} />
              </>
            ) : (
              <div className="empty-state compact">Select a registered device to view its details.</div>
            )}
          </article>
        </section>

        <AlertsPanel devices={devices} />

        <section className="panel telemetry-panel" id="telemetry">
          <div className="panel-heading telemetry-heading">
            <div>
              <div className="eyebrow">TIME-SERIES EXPLORER</div>
              <h2>Telemetry analytics</h2>
              <p>Up to five metrics reported by the selected terminal.</p>
            </div>
            <div className="chart-controls">
              <label>
                <span>Time range</span>
                <select value={range} onChange={(event) => setRange(event.target.value)}>
                  {RANGES.map((item) => <option key={item.value} value={item.value}>{item.label}</option>)}
                </select>
              </label>
            </div>
          </div>

          {!selectedId ? (
            <div className="empty-state compact">Select a terminal to view telemetry charts.</div>
          ) : metricNames.length === 0 ? (
            <div className="empty-state compact">
              <strong>No telemetry metrics yet</strong>
              <p>Charts appear when the selected terminal reports real readings.</p>
            </div>
          ) : (
            <div className="telemetry-chart-grid">
              {metricNames.slice(0, 5).map((metricName, index) => {
                const points = telemetry
                  .filter((item) => item.name === metricName)
                  .map((item) => ({
                    timestamp: item.receivedAt,
                    time: formatTime(item.receivedAt),
                    value: Number(item.value),
                    unit: item.unit || ''
                  }))
                  .filter((item) => Number.isFinite(item.value));

                const latest = latestByMetric.find((item) => item.name === metricName);
                const freshness = telemetryFreshness(latest, now);
                const chartId = `telemetryFill${index}`;

                return (
                  <article className="telemetry-chart-card" key={metricName}>
                    <div className="telemetry-chart-heading">
                      <div>
                        <h3>{metricName}</h3>
                        <small className={`metric-freshness freshness-${freshness.status}`}>
                          {freshness.label} · {freshness.age}
                        </small>
                      </div>
                      <div className="telemetry-chart-value">
                        {latest ? Number(latest.value).toLocaleString(undefined, { maximumFractionDigits: 3 }) : '—'}
                        {latest?.unit && <span> {latest.unit}</span>}
                      </div>
                    </div>
                    <div className="chart-points">{points.length} data points</div>
                    <div className="chart-wrap">
                      {points.length === 0 ? (
                        <div className="empty-state chart-empty">
                          <strong>No history in this range</strong>
                          <p>Wait for readings or select a longer range.</p>
                        </div>
                      ) : (
                        <ResponsiveContainer width="100%" height="100%">
                          <AreaChart data={points} margin={{ top: 12, right: 12, left: 0, bottom: 0 }}>
                            <defs>
                              <linearGradient id={chartId} x1="0" y1="0" x2="0" y2="1">
                                <stop offset="0%" stopColor="#22c99a" stopOpacity={0.24} />
                                <stop offset="95%" stopColor="#22c99a" stopOpacity={0.01} />
                              </linearGradient>
                            </defs>
                            <CartesianGrid stroke="#e8edf2" strokeDasharray="4 5" vertical={false} />
                            <XAxis dataKey="time" tick={{ fill: '#8491a2', fontSize: 10 }} tickLine={false} axisLine={false} minTickGap={28} />
                            <YAxis width={48} tick={{ fill: '#8491a2', fontSize: 10 }} tickLine={false} axisLine={false} domain={['auto', 'auto']} />
                            <Tooltip
                              contentStyle={{ border: '1px solid #e3e9ef', borderRadius: 10 }}
                              formatter={(value, _name, item) => [
                                `${Number(value).toLocaleString()} ${item.payload.unit}`.trim(),
                                metricName
                              ]}
                            />
                            <Area
                              type="monotone"
                              dataKey="value"
                              stroke="#12a980"
                              strokeWidth={2}
                              fill={`url(#${chartId})`}
                              dot={points.length < 25 ? { r: 2, fill: '#12a980', strokeWidth: 0 } : false}
                              activeDot={{ r: 4, stroke: '#fff', strokeWidth: 2 }}
                              isAnimationActive={false}
                            />
                          </AreaChart>
                        </ResponsiveContainer>
                      )}
                    </div>
                  </article>
                );
              })}
            </div>
          )}

          <div className="metric-strip">
            {latestByMetric.length === 0 ? (
              <span className="muted-text">No metric summaries available.</span>
            ) : latestByMetric.map((item) => (
              <button
                className={`metric-chip ${selectedMetric === item.name ? 'active' : ''}`}
                key={item.name}
                onClick={() => setSelectedMetric(item.name)}
              >
                <MetricIcon />
                <span>
                  <small>{item.name}</small>
                  <strong>{Number(item.value).toLocaleString()} {item.unit}</strong>
                  <small className={`metric-freshness freshness-${telemetryFreshness(item, now).status}`}>
                    {telemetryFreshness(item, now).label} · {telemetryFreshness(item, now).age}
                  </small>
                </span>
              </button>
            ))}
          </div>
        </section>

        <section className="panel connections-panel" id="connections">
          <div className="panel-heading">
            <div>
              <h2>Connection history</h2>
              <p>Recent Wi-Fi and MQTT state changes for the selected terminal.</p>
            </div>
            <span className="count-badge">{connections.length} events</span>
          </div>

          {!selectedId ? (
            <div className="empty-state compact">Select a device to view connection events.</div>
          ) : connections.length === 0 ? (
            <div className="empty-state compact">No connection events recorded for this terminal.</div>
          ) : (
            <div className="table-scroll">
              <table>
                <thead>
                  <tr><th>TYPE</th><th>STATE</th><th>DETAILS</th><th>TIME (UTC)</th></tr>
                </thead>
                <tbody>
                  {connections.map((event, index) => (
                    <tr key={`${event.type}-${event.occurredAt}-${index}`}>
                      <td><span className="type-label">{event.type}</span></td>
                      <td><StatusPill status={event.state === 'connected' ? 'online' : 'offline'} /></td>
                      <td className="details-cell">{event.details || '—'}</td>
                      <td>{event.occurredAt ? new Date(event.occurredAt).toLocaleString([], {
                        dateStyle: 'medium',
                        timeStyle: 'short',
                        timeZone: 'UTC'
                      }) : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </section>

        <footer className="page-footer">
          <span>IoT Commander · Monitoring console</span>
          <span>Refresh interval 15s · {new Date(now).toLocaleDateString()}</span>
        </footer>
      </section>
    </main>
  );
}
