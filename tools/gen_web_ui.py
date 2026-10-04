#!/usr/bin/env python3
"""Generate nx_web_ui.h from HTML content as a byte array to comply with ISO C11 -Wpedantic."""

HTML_CONTENT = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>NexusSearch - Modern Engine UI</title>
  <style>
    :root {
      --bg-primary: #0a0f1d;
      --bg-surface: #131b2e;
      --bg-card: #1c2641;
      --border: #2a375a;
      --text-main: #f1f5f9;
      --text-muted: #94a3b8;
      --accent: #6366f1;
      --accent-hover: #4f46e5;
      --accent-glow: rgba(99, 102, 241, 0.25);
      --success: #10b981;
      --warning: #f59e0b;
      --code-bg: #090d16;
      --radius: 10px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }
    body { background-color: var(--bg-primary); color: var(--text-main); min-height: 100vh; padding: 24px; }
    .container { max-width: 1100px; margin: 0 auto; }
    header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 28px; padding-bottom: 20px; border-bottom: 1px solid var(--border); }
    .logo { display: flex; align-items: center; gap: 12px; font-size: 22px; font-weight: 700; color: #fff; letter-spacing: -0.5px; }
    .logo-badge { background: linear-gradient(135deg, #6366f1, #a855f7); color: #fff; font-size: 11px; padding: 3px 8px; border-radius: 6px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
    .header-stats { display: flex; gap: 16px; font-size: 13px; color: var(--text-muted); }
    .stat-pill { background: var(--bg-surface); padding: 6px 14px; border-radius: 20px; border: 1px solid var(--border); }
    .stat-pill strong { color: var(--text-main); }
    .search-box { position: relative; margin-bottom: 16px; }
    .search-input-wrapper { display: flex; gap: 10px; background: var(--bg-surface); border: 2px solid var(--border); border-radius: 12px; padding: 6px 8px; transition: all 0.2s ease; }
    .search-input-wrapper:focus-within { border-color: var(--accent); box-shadow: 0 0 0 4px var(--accent-glow); }
    .search-input { flex: 1; background: transparent; border: none; outline: none; color: #fff; font-size: 16px; padding: 8px 12px; }
    .search-btn { background: var(--accent); color: #fff; border: none; border-radius: 8px; padding: 0 20px; font-size: 14px; font-weight: 600; cursor: pointer; transition: background 0.15s; }
    .search-btn:hover { background: var(--accent-hover); }
    .controls { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 12px; margin-bottom: 24px; }
    .presets { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
    .preset-label { font-size: 12px; color: var(--text-muted); font-weight: 600; margin-right: 4px; }
    .preset-chip { background: var(--bg-surface); border: 1px solid var(--border); color: var(--text-muted); padding: 4px 10px; border-radius: 6px; font-size: 12px; cursor: pointer; transition: all 0.15s; font-family: monospace; }
    .preset-chip:hover { border-color: var(--accent); color: var(--text-main); background: var(--bg-card); }
    .toggle-group { display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--text-muted); }
    .switch { position: relative; display: inline-block; width: 36px; height: 20px; }
    .switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: var(--border); transition: .2s; border-radius: 20px; }
    .slider:before { position: absolute; content: ''; height: 14px; width: 14px; left: 3px; bottom: 3px; background-color: white; transition: .2s; border-radius: 50%; }
    input:checked + .slider { background-color: var(--accent); }
    input:checked + .slider:before { transform: translateX(16px); }
    .tabs { display: flex; gap: 20px; margin-bottom: 20px; border-bottom: 1px solid var(--border); }
    .tab-btn { background: none; border: none; color: var(--text-muted); font-size: 14px; font-weight: 600; padding: 10px 0; cursor: pointer; position: relative; }
    .tab-btn.active { color: var(--text-main); }
    .tab-btn.active::after { content: ''; position: absolute; bottom: -1px; left: 0; right: 0; height: 2px; background: var(--accent); }
    .metrics-bar { display: flex; gap: 20px; background: var(--bg-surface); border: 1px solid var(--border); padding: 12px 18px; border-radius: 8px; margin-bottom: 20px; font-size: 13px; }
    .metric-item { display: flex; align-items: center; gap: 6px; }
    .metric-item strong { color: var(--success); }
    .results-container { display: flex; flex-direction: column; gap: 14px; }
    .result-card { background: var(--bg-surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 18px; transition: border-color 0.15s; }
    .result-card:hover { border-color: rgba(99, 102, 241, 0.4); }
    .result-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
    .result-id { font-size: 15px; font-weight: 700; color: #fff; display: flex; align-items: center; gap: 8px; }
    .row-badge { font-size: 11px; background: var(--bg-card); color: var(--text-muted); padding: 2px 6px; border-radius: 4px; font-family: monospace; }
    .score-badge { display: flex; align-items: center; gap: 8px; font-size: 12px; font-weight: 600; color: var(--success); background: rgba(16, 185, 129, 0.12); padding: 3px 10px; border-radius: 12px; border: 1px solid rgba(16, 185, 129, 0.2); }
    .score-bar { width: 50px; height: 5px; background: var(--bg-card); border-radius: 3px; overflow: hidden; }
    .score-fill { height: 100%; background: var(--success); }
    .result-body { color: var(--text-muted); font-size: 14px; line-height: 1.5; margin-bottom: 12px; }
    .result-fields { display: flex; flex-wrap: wrap; gap: 8px; font-size: 12px; }
    .field-tag { background: var(--bg-card); padding: 3px 8px; border-radius: 4px; border: 1px solid var(--border); }
    .field-name { color: var(--text-muted); }
    .field-val { color: var(--text-main); font-weight: 500; }
    .json-toggle { background: transparent; border: none; color: var(--accent); font-size: 12px; cursor: pointer; margin-top: 10px; display: inline-block; }
    .raw-json { display: none; background: var(--code-bg); border: 1px solid var(--border); border-radius: 6px; padding: 12px; font-family: monospace; font-size: 12px; color: #38bdf8; margin-top: 10px; white-space: pre-wrap; word-break: break-all; }
    .schema-table { width: 100%; border-collapse: collapse; background: var(--bg-surface); border-radius: var(--radius); overflow: hidden; border: 1px solid var(--border); }
    .schema-table th, .schema-table td { padding: 12px 16px; text-align: left; font-size: 13px; border-bottom: 1px solid var(--border); }
    .schema-table th { background: var(--bg-card); color: var(--text-muted); font-weight: 600; text-transform: uppercase; font-size: 11px; letter-spacing: 0.5px; }
    .type-badge { font-size: 11px; padding: 2px 8px; border-radius: 4px; font-weight: 600; font-family: monospace; }
    .type-int { background: rgba(59, 130, 246, 0.2); color: #60a5fa; }
    .type-float { background: rgba(168, 85, 247, 0.2); color: #c084fc; }
    .type-bool { background: rgba(245, 158, 11, 0.2); color: #fbbf24; }
    .type-text { background: rgba(16, 185, 129, 0.2); color: #34d399; }
    .type-vector { background: rgba(236, 72, 153, 0.2); color: #f472b6; }
    .explain-view { background: var(--bg-surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 20px; font-family: monospace; font-size: 13px; color: #38bdf8; white-space: pre-wrap; }
    .empty-state { text-align: center; padding: 60px 20px; color: var(--text-muted); }
    .empty-icon { font-size: 40px; margin-bottom: 12px; }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="logo">
        NexusSearch
        <span class="logo-badge">v0.2</span>
      </div>
      <div class="header-stats" id="headerStats">
        <div class="stat-pill">Rows: <strong id="statRows">-</strong></div>
        <div class="stat-pill">Fields: <strong id="statFields">-</strong></div>
      </div>
    </header>
    <div class="search-box">
      <div class="search-input-wrapper">
        <input type="text" id="queryInput" class="search-input" placeholder="Search with NexusQL (e.g. title:search AND year:>=2025)..." autofocus>
        <button class="search-btn" id="searchBtn">Search</button>
      </div>
    </div>
    <div class="controls">
      <div class="presets">
        <span class="preset-label">Try:</span>
        <span class="preset-chip" data-q="search">search</span>
        <span class="preset-chip" data-q="active:true AND year:>=2025">year:>=2025 AND active:true</span>
        <span class="preset-chip" data-q="title:prefix(sea)">title:prefix(sea)</span>
        <span class="preset-chip" data-q="title:fuzzy(serch,1)">title:fuzzy(serch,1)</span>
        <span class="preset-chip" data-q="embedding:[1,0,0] LIMIT 3">embedding:[1,0,0]</span>
        <span class="preset-chip" data-q="* SORT BY year DESC">* SORT BY year DESC</span>
      </div>
      <div class="toggle-group">
        <label class="switch">
          <input type="checkbox" id="scanToggle">
          <span class="slider"></span>
        </label>
        <span>Scan Oracle Mode</span>
      </div>
    </div>
    <div class="tabs">
      <button class="tab-btn active" data-tab="tabResults">Results</button>
      <button class="tab-btn" data-tab="tabExplain">Query Plan</button>
      <button class="tab-btn" data-tab="tabSchema">Snapshot Schema</button>
    </div>
    <div id="tabResults">
      <div class="metrics-bar" id="metricsBar" style="display: none;">
        <div class="metric-item">Matches: <strong id="metricMatches">0</strong></div>
        <div class="metric-item">Work Units: <strong id="metricWork">0</strong></div>
        <div class="metric-item">Indexed Scans: <strong id="metricIndexed">0</strong></div>
        <div class="metric-item">Scanned Cells: <strong id="metricScanned">0</strong></div>
        <div class="metric-item">Vectors Scored: <strong id="metricVectors">0</strong></div>
        <div class="metric-item">Latency: <strong id="metricTime">0ms</strong></div>
      </div>
      <div class="results-container" id="resultsList">
        <div class="empty-state">
          <div class="empty-icon">⚡</div>
          <h3>Enter a query or select a preset above</h3>
          <p style="margin-top: 6px;">Supports boolean logic, bit-sliced numeric filters, BM25 text, regex, and vector embeddings.</p>
        </div>
      </div>
    </div>
    <div id="tabExplain" style="display: none;">
      <div class="explain-view" id="explainContent">Run a search to view the execution plan...</div>
    </div>
    <div id="tabSchema" style="display: none;">
      <table class="schema-table">
        <thead><tr><th>Field Name</th><th>Data Type</th><th>Details</th></tr></thead>
        <tbody id="schemaTbody"></tbody>
      </table>
    </div>
  </div>
  <script>
    let currentSchema = null;
    async function loadStats() {
      try {
        const res = await fetch('/api/stats');
        if (!res.ok) return;
        const data = await res.json();
        currentSchema = data;
        document.getElementById('statRows').textContent = data.rows.toLocaleString();
        document.getElementById('statFields').textContent = (data.fields ? data.fields.length : 0).toString();
        const tbody = document.getElementById('schemaTbody');
        tbody.innerHTML = '';
        (data.fields || []).forEach(f => {
          const tr = document.createElement('tr');
          const typeClass = 'type-' + f.type;
          const details = f.type === 'vector' ? (f.dimensions + ' dimensions') : '-';
          tr.innerHTML = `<td><strong>${f.name}</strong></td><td><span class="type-badge ${typeClass}">${f.type}</span></td><td>${details}</td>`;
          tbody.appendChild(tr);
        });
      } catch (e) { console.error('Failed to load stats', e); }
    }
    async function doSearch() {
      const q = document.getElementById('queryInput').value.trim();
      if (!q) return;
      const scan = document.getElementById('scanToggle').checked;
      const start = performance.now();
      try {
        const res = await fetch(`/api/search?q=${encodeURIComponent(q)}&scan=${scan ? 1 : 0}`);
        const elapsed = (performance.now() - start).toFixed(1);
        const data = await res.json();
        if (!res.ok) {
          document.getElementById('resultsList').innerHTML = `<div class="result-card" style="border-color: #ef4444;"><strong style="color:#ef4444;">Error:</strong> ${data.error || 'Search failed'}</div>`;
          return;
        }
        renderResults(data, elapsed);
        loadExplain(q);
      } catch (e) {
        document.getElementById('resultsList').innerHTML = `<div class="result-card" style="border-color: #ef4444;"><strong style="color:#ef4444;">Request Error:</strong> ${e.message}</div>`;
      }
    }
    async function loadExplain(q) {
      try {
        const res = await fetch(`/api/explain?q=${encodeURIComponent(q)}`);
        if (res.ok) {
          const data = await res.json();
          document.getElementById('explainContent').textContent = JSON.stringify(data, null, 2);
        }
      } catch (e) {}
    }
    function renderResults(data, elapsed) {
      const metricsBar = document.getElementById('metricsBar');
      metricsBar.style.display = 'flex';
      document.getElementById('metricMatches').textContent = `${data.count} / ${data.total}`;
      document.getElementById('metricWork').textContent = (data.work || 0).toLocaleString();
      document.getElementById('metricIndexed').textContent = (data.numeric_indexes || 0).toLocaleString();
      document.getElementById('metricScanned').textContent = (data.scanned_cells || 0).toLocaleString();
      document.getElementById('metricVectors').textContent = (data.vectors_scored || 0).toLocaleString();
      document.getElementById('metricTime').textContent = `${elapsed}ms`;
      const container = document.getElementById('resultsList');
      if (!data.hits || data.hits.length === 0) {
        container.innerHTML = `<div class="empty-state"><div class="empty-icon">🔍</div><h3>No documents matched your query</h3><p style="margin-top:6px;">Try adjusting your filters or terms.</p></div>`;
        return;
      }
      let html = '';
      const maxScore = Math.max(...data.hits.map(h => h.score || 0), 0.001);
      data.hits.forEach((hit, idx) => {
        let doc = {};
        try { doc = JSON.parse(hit.document); } catch (e) {}
        const scorePercent = Math.min(100, Math.max(5, Math.round(((hit.score || 0) / maxScore) * 100)));
        let fieldsHtml = '';
        for (const [k, v] of Object.entries(doc)) {
          if (k === '_id' || k === 'body' || k === 'title') continue;
          const valStr = typeof v === 'object' ? JSON.stringify(v) : String(v);
          fieldsHtml += `<span class="field-tag"><span class="field-name">${k}:</span> <span class="field-val">${valStr}</span></span>`;
        }
        const bodyText = doc.body ? `<div class="result-body">${doc.body}</div>` : '';
        const titleText = doc.title ? `<div style="font-size:16px;font-weight:600;color:#fff;margin-bottom:6px;">${doc.title}</div>` : '';
        const scoreDisplay = typeof hit.score === 'number' ? hit.score.toFixed(4) : '-';
        html += `
          <div class="result-card">
            <div class="result-header">
              <div class="result-id">
                <span>${hit._id}</span>
                <span class="row-badge">row ${hit.row}</span>
              </div>
              <div class="score-badge">
                <div class="score-bar"><div class="score-fill" style="width: ${scorePercent}%"></div></div>
                <span>${scoreDisplay}</span>
              </div>
            </div>
            ${titleText}
            ${bodyText}
            <div class="result-fields">${fieldsHtml}</div>
            <button class="json-toggle" onclick="this.nextElementSibling.style.display = this.nextElementSibling.style.display === 'block' ? 'none' : 'block'">View JSON &darr;</button>
            <div class="raw-json">${JSON.stringify(doc, null, 2)}</div>
          </div>`;
      });
      container.innerHTML = html;
    }
    document.querySelectorAll('.preset-chip').forEach(chip => {
      chip.addEventListener('click', () => {
        document.getElementById('queryInput').value = chip.getAttribute('data-q');
        doSearch();
      });
    });
    document.getElementById('searchBtn').addEventListener('click', doSearch);
    document.getElementById('queryInput').addEventListener('keydown', e => { if (e.key === 'Enter') doSearch(); });
    document.querySelectorAll('.tab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        ['tabResults', 'tabExplain', 'tabSchema'].forEach(id => document.getElementById(id).style.display = 'none');
        document.getElementById(btn.getAttribute('data-tab')).style.display = 'block';
      });
    });
    loadStats();
  </script>
</body>
</html>
"""

def main():
    raw = HTML_CONTENT.strip().encode("utf-8")
    lines = [
        "/* Generated by tools/gen_web_ui.py - do not edit directly */",
        "#ifndef NX_WEB_UI_H",
        "#define NX_WEB_UI_H",
        "#include <stddef.h>",
        "",
        "static const unsigned char nx_web_ui_html[] = {",
    ]
    for i in range(0, len(raw), 16):
        chunk = raw[i:i+16]
        hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_bytes},")
    lines.append("    0x00")
    lines.append("};")
    lines.append(f"static const size_t nx_web_ui_html_len = {len(raw)};")
    lines.append("#endif")
    lines.append("")

    with open("src/server/nx_web_ui.h", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"Generated src/server/nx_web_ui.h with {len(raw)} bytes.")

if __name__ == "__main__":
    main()
