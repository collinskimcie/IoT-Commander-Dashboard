import { useEffect, useMemo, useState } from 'react';
import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis
} from 'recharts';
import './fleet.css';

const STATUS_COLOR = {
  online: '#12a980',
  stale: '#d99a2b',
  offline: '#d84a4a',
  never_seen: '#8491a2'
};

const statusOf = (device) => device.status || 'never_seen';
const labelOf = (device) => device.name || device.id;

function formatDuration(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return '—';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function formatKb(bytes) {
  return Number.isFinite(bytes) ? `${Math.round(bytes / 1024)} KB` : '—';
}

function signalLevel(rssi) {
  if (!Number.isFinite(rssi)) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

function SignalBars({ rssi }) {
  const level = signalLevel(rssi);
  return (
    <span
      className="signal-bars"
      title={Number.isFinite(rssi) ? `${rssi} dBm` : 'No signal data'}
    >
      {[1, 2, 3, 4].map((bar) => (
        <span key={bar} className={bar <= level ? 'on' : ''} style={{ height: 4 + bar * 3 }} />
      ))}
    </span>
  );
}

function MeshView({ devices, selectedId, onSelect }) {
  const size = 280;
  const center = size / 2;
  const radius = 98;
  const nodes = devices.slice(0, 12).map((device, index, list) => {
    const angle = (2 * Math.PI * index) / list.length - Math.PI / 2;
    return {
      device,
      x: center + radius * Math.cos(angle),
      y: center + radius * Math.sin(angle)
    };
  });

  return (
    <svg viewBox={`0 0 ${size} ${size}`} className="mesh-svg" role="img" aria-label="Device mesh">
      {nodes.map(({ device, x, y }) => (
        <line
          key={`link-${device.id}`}
          x1={center}
          y1={center}
          x2={x}
          y2={y}
          stroke={STATUS_COLOR[statusOf(device)]}
          strokeWidth="1.5"
          strokeDasharray={statusOf(device) === 'online' ? undefined : '4 4'}
        />
      ))}
      <circle cx={center} cy={center} r="22" className="mesh-hub" />
      <text x={center} y={center + 4} textAnchor="middle" className="mesh-hub-label">API</text>
      {nodes.map(({ device, x, y }) => (
        <g
          key={device.id}
          className="mesh-node"
          tabIndex={0}
          role="button"
          aria-label={`Select ${labelOf(device)}`}
          onClick={() => onSelect(device.id)}
          onKeyDown={(event) => {
            if (event.key === 'Enter' || event.key === ' ') onSelect(device.id);
          }}
        >
          <circle
            cx={x}
            cy={y}
            r="12"
            fill={STATUS_COLOR[statusOf(device)]}
            stroke={device.id === selectedId ? '#0b1220' : '#fff'}
            strokeWidth={device.id === selectedId ? 3 : 2}
          />
          <text x={x} y={y + 27} textAnchor="middle" className="mesh-label">
            {labelOf(device).slice(0, 10)}
          </text>
        </g>
      ))}
    </svg>
  );
}

export function FleetOverview({ devices, selectedId, onSelect }) {
  const groups = useMemo(() => {
    const map = new Map();
    for (const device of devices) {
      const key = device.groupName || 'Ungrouped';
      if (!map.has(key)) map.set(key, []);
      map.get(key).push(device);
    }
    return [...map.entries()];
  }, [devices]);

  if (devices.length === 0) return null;

  return (
    <section className="panel fleet-panel" id="fleet">
      <div className="panel-heading">
        <div>
          <h2>Fleet overview</h2>
          <p>Every terminal at a glance, with its latest readings.</p>
        </div>
        <span className="count-badge">{groups.length} group{groups.length === 1 ? '' : 's'}</span>
      </div>
      <div className="fleet-layout">
        <div className="fleet-groups">
          {groups.map(([group, members]) => (
            <div key={group} className="fleet-group">
              <div className="fleet-group-title">{group}</div>
              <div className="fleet-tiles">
                {members.map((device) => {
                  const status = statusOf(device);
                  const online = status === 'online';
                  return (
                    <button
                      key={device.id}
                      className={`fleet-tile ${device.id === selectedId ? 'selected' : ''}`}
                      onClick={() => onSelect(device.id)}
                    >
                      <span className="tile-top">
                        <span className="tile-dot" style={{ background: STATUS_COLOR[status] }} />
                        <strong>{labelOf(device)}</strong>
                        <SignalBars rssi={online ? device.rssi : undefined} />
                      </span>
                      <span className="tile-meta">
                        {online ? `Up ${formatDuration(device.uptimeSeconds)}` : status.replace('_', ' ')}
                        {Number.isFinite(device.availability24h) && ` · ${device.availability24h}% (24h)`}
                      </span>
                      <span className="quick-chips">
                        {(device.latest || []).slice(0, 3).map((reading) => (
                          <span key={reading.name} className="quick-chip">
                            {reading.name}{' '}
                            <b>
                              {Number(reading.value).toLocaleString(undefined, { maximumFractionDigits: 2 })}
                              {reading.unit ? ` ${reading.unit}` : ''}
                            </b>
                          </span>
                        ))}
                        {(!device.latest || device.latest.length === 0) && (
                          <span className="quick-chip muted">No readings yet</span>
                        )}
                      </span>
                    </button>
                  );
                })}
              </div>
            </div>
          ))}
        </div>
        <MeshView devices={devices} selectedId={selectedId} onSelect={onSelect} />
      </div>
    </section>
  );
}

function Item({ label, value }) {
  return (
    <div className="health-item">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function FirmwarePanel({ device, onChanged }) {
  const deviceId = device?.id;
  const online = statusOf(device) === 'online';
  const [images, setImages] = useState([]);
  const [offer, setOffer] = useState(null);
  const [selected, setSelected] = useState('');
  const [file, setFile] = useState(null);
  const [inputKey, setInputKey] = useState(0);
  const [version, setVersion] = useState('');
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');

  async function call(path, options) {
    const response = await fetch(path, options);
    if (!response.ok) {
      let detail = `HTTP ${response.status}`;
      try {
        const body = await response.json();
        if (body && body.error) detail = body.error;
      } catch {
        // keep the status text
      }
      throw new Error(detail);
    }
    return response.status === 204 ? null : response.json();
  }

  async function refresh() {
    try {
      const [list, current] = await Promise.all([
        call('/api/v1/firmware'),
        call(`/api/v1/terminals/${encodeURIComponent(deviceId)}/firmware`)
      ]);
      setImages(Array.isArray(list) ? list : []);
      setOffer(current && current.available ? current : null);
    } catch {
      // the panel keeps what it had if a refresh fails
    }
  }

  useEffect(() => {
    if (!deviceId) return undefined;
    refresh();
    const timer = window.setInterval(refresh, 10000);
    return () => window.clearInterval(timer);
  }, [deviceId]);

  async function upload() {
    if (!file || !version.trim()) return;
    setBusy(true);
    setMessage('');
    try {
      const created = await call(`/api/v1/firmware?version=${encodeURIComponent(version.trim())}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: file
      });
      setMessage(`Uploaded ${created.version} (${Math.round(created.size / 1024)} KB)`);
      setFile(null);
      setVersion('');
      setInputKey((key) => key + 1);
      setSelected(String(created.id));
      await refresh();
    } catch (err) {
      setMessage(`Upload failed: ${err.message}`);
    } finally {
      setBusy(false);
    }
  }

  async function request(imageId) {
    setBusy(true);
    setMessage('');
    try {
      await call(`/api/v1/terminals/${encodeURIComponent(deviceId)}/firmware`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ imageId })
      });
      setMessage(imageId === null ? 'Update cancelled' : 'Requested. The terminal picks it up within about a minute.');
      await refresh();
      if (onChanged) onChanged();
    } catch (err) {
      setMessage(`Could not request: ${err.message}`);
    } finally {
      setBusy(false);
    }
  }

  function startUpdate() {
    const image = images.find((item) => String(item.id) === selected);
    if (!image) return;
    const ok = window.confirm(
      `Update ${labelOf(device)} to ${image.version}?\n\nThe terminal downloads the image, restarts, and rolls back on its own if the new image cannot reach the backend within 2 minutes.`
    );
    if (ok) request(image.id);
  }

  return (
    <div className="firmware-panel">
      <div className="health-title">Firmware update</div>
      <div className="firmware-row">
        <span>Running</span>
        <strong>{device.firmware || '—'}</strong>
      </div>
      {offer && (
        <div className="firmware-row">
          <span>Requested</span>
          <strong>{offer.version}</strong>
          <button type="button" disabled={busy} onClick={() => request(null)}>Cancel</button>
        </div>
      )}
      <div className="firmware-row">
        <select value={selected} onChange={(event) => setSelected(event.target.value)}>
          <option value="">Choose an uploaded image…</option>
          {images.map((image) => (
            <option key={image.id} value={image.id}>
              {image.version} · {Math.round(image.size / 1024)} KB · {new Date(image.uploadedAt).toLocaleString()}
            </option>
          ))}
        </select>
        <button type="button" disabled={busy || !selected || !online || Boolean(offer)} onClick={startUpdate}>
          Request update
        </button>
      </div>
      {!online && <small>The terminal must be online to start an update.</small>}
      <div className="firmware-row">
        <input key={inputKey} type="file" accept=".bin" onChange={(event) => setFile(event.target.files?.[0] || null)} />
        <input
          value={version}
          maxLength={24}
          placeholder="Version, e.g. 0.5.1"
          onChange={(event) => setVersion(event.target.value)}
        />
        <button type="button" disabled={busy || !file || !version.trim()} onClick={upload}>Upload</button>
      </div>
      {message && <small>{message}</small>}
    </div>
  );
}

export function HealthPanel({ device, onChanged }) {
  const [history, setHistory] = useState([]);
  const [name, setName] = useState('');
  const [group, setGroup] = useState('');
  const [saving, setSaving] = useState(false);
  const [message, setMessage] = useState('');
  const deviceId = device?.id;

  useEffect(() => {
    setName(device?.name || '');
    setGroup(device?.groupName || '');
    setMessage('');
  }, [deviceId, device?.name, device?.groupName]);

  useEffect(() => {
    if (!deviceId) {
      setHistory([]);
      return undefined;
    }
    const controller = new AbortController();

    async function load() {
      try {
        const response = await fetch(
          `/api/v1/terminals/${encodeURIComponent(deviceId)}/health?hours=24`,
          { headers: { Accept: 'application/json' }, signal: controller.signal }
        );
        if (!response.ok) return;
        const rows = await response.json();
        if (!Array.isArray(rows)) return;
        setHistory(
          rows
            .filter((row) => Number.isFinite(row.rssi))
            .map((row) => ({
              time: new Date(row.receivedAt).toLocaleTimeString([], {
                hour: '2-digit',
                minute: '2-digit'
              }),
              rssi: row.rssi
            }))
        );
      } catch {
        // The panel simply stays empty if the request fails or is aborted.
      }
    }

    load();
    const timer = window.setInterval(load, 30000);
    return () => {
      controller.abort();
      window.clearInterval(timer);
    };
  }, [deviceId]);

  if (!device) return null;

  async function save(event) {
    event.preventDefault();
    setSaving(true);
    setMessage('');
    try {
      const response = await fetch(`/api/v1/terminals/${encodeURIComponent(deviceId)}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, groupName: group })
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      setMessage('Saved');
      if (onChanged) onChanged();
    } catch (err) {
      setMessage(`Could not save: ${err.message}`);
    } finally {
      setSaving(false);
    }
  }

  const online = statusOf(device) === 'online';

  return (
    <div className="health-panel">
      <div className="health-title">Device health</div>
      <div className="health-grid">
        <Item label="Firmware" value={device.firmware || '—'} />
        <Item label="Uptime" value={online ? formatDuration(device.uptimeSeconds) : 'Offline'} />
        <Item
          label="Last boot"
          value={
            device.lastBootAt
              ? new Date(device.lastBootAt).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })
              : '—'
          }
        />
        <Item label="Boots seen" value={device.bootCount ?? '—'} />
        <Item label="Reset reason" value={device.resetReason || '—'} />
        <Item
          label="Availability (24h)"
          value={Number.isFinite(device.availability24h) ? `${device.availability24h}%` : '—'}
        />
        <Item
          label="Free heap"
          value={`${formatKb(device.heapFree)} (block ${formatKb(device.heapMaxBlock)})`}
        />
        <Item label="Free PSRAM" value={formatKb(device.psramFree)} />
        <Item
          label="Wi-Fi signal"
          value={
            <>
              <SignalBars rssi={device.rssi} />{' '}
              {Number.isFinite(device.rssi) ? `${device.rssi} dBm` : '—'}
            </>
          }
        />
      </div>

      <div className="health-title">Wi-Fi signal, last 24 hours</div>
      <div className="health-chart">
        {history.length < 2 ? (
          <div className="muted-text">Signal history appears after a few heartbeats.</div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <AreaChart data={history} margin={{ top: 8, right: 8, left: 0, bottom: 0 }}>
              <CartesianGrid stroke="#e8edf2" strokeDasharray="4 5" vertical={false} />
              <XAxis dataKey="time" tick={{ fill: '#8491a2', fontSize: 10 }} tickLine={false} axisLine={false} minTickGap={28} />
              <YAxis width={40} domain={[-100, -30]} tick={{ fill: '#8491a2', fontSize: 10 }} tickLine={false} axisLine={false} />
              <Tooltip formatter={(value) => [`${value} dBm`, 'Signal']} />
              <Area type="monotone" dataKey="rssi" stroke="#12a980" strokeWidth={2} fill="#22c99a" fillOpacity={0.15} dot={false} isAnimationActive={false} />
            </AreaChart>
          </ResponsiveContainer>
        )}
      </div>

      <FirmwarePanel device={device} onChanged={onChanged} />

      <form className="health-form" onSubmit={save}>
        <label>
          <span>Name</span>
          <input value={name} maxLength={40} onChange={(event) => setName(event.target.value)} placeholder="e.g. Greenhouse" />
        </label>
        <label>
          <span>Group</span>
          <input value={group} maxLength={40} onChange={(event) => setGroup(event.target.value)} placeholder="e.g. Site A" />
        </label>
        <button type="submit" disabled={saving}>{saving ? 'Saving…' : 'Save'}</button>
        {message && <small>{message}</small>}
      </form>
    </div>
  );
}
