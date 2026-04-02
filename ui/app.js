async function loadReport() {
  try {
    const response = await fetch("./data/benchmark_report.json", { cache: "no-store" });
    if (!response.ok) {
      throw new Error("benchmark_report.json load failed");
    }
    return response.json();
  } catch (error) {
    if (window.BENCHMARK_REPORT) {
      return window.BENCHMARK_REPORT;
    }
    throw error;
  }
}

function formatSpeedup(value) {
  return `${value.toFixed(2)}x`;
}

function renderSummary(report) {
  const comparisons = report.comparisons || [];
  const best = comparisons.reduce((max, item) => Math.max(max, item.speedup), 0);
  const avg =
    comparisons.reduce((sum, item) => sum + item.speedup, 0) / (comparisons.length || 1);
  const topItem = comparisons.reduce(
    (bestItem, item) => (!bestItem || item.speedup > bestItem.speedup ? item : bestItem),
    null
  );

  document.getElementById("best-speedup").textContent = formatSpeedup(best);
  document.getElementById("avg-speedup").textContent = formatSpeedup(avg);
  document.getElementById("case-count").textContent = String(comparisons.length);
  document.getElementById("scenario-count").textContent = String(comparisons.length);
  document.getElementById("top-case").textContent = topItem
    ? `${topItem.threads}T / ${topItem.allocSize}B`
    : "--";
}

function createBar(label, value, maxValue, type) {
  const row = document.createElement("div");
  row.className = "bar-row";

  const labelEl = document.createElement("div");
  labelEl.className = "bar-label";
  labelEl.textContent = label;

  const track = document.createElement("div");
  track.className = "bar-track";

  const fill = document.createElement("div");
  fill.className = `bar-fill ${type}`;
  fill.style.width = `${Math.max(8, (value / maxValue) * 100)}%`;
  track.appendChild(fill);

  const valueEl = document.createElement("div");
  valueEl.className = "bar-value";
  valueEl.textContent = `${value} ms`;

  row.append(labelEl, track, valueEl);
  return row;
}

function renderCharts(report) {
  const chartArea = document.getElementById("chart-area");
  chartArea.innerHTML = "";

  const sorted = [...(report.comparisons || [])].sort((a, b) => b.speedup - a.speedup);
  const featured = sorted.slice(0, 12);

  for (const item of featured) {
    const maxValue = Math.max(item.baselineMs, item.poolMs, 1);
    const card = document.createElement("div");
    card.className = "chart-card";

    const title = document.createElement("div");
    title.className = "chart-title";
    title.innerHTML = `
      <h4>${item.threads} 线程 / ${item.allocSize} Bytes</h4>
      <span>${item.iterations.toLocaleString()} 次迭代 · 加速 ${formatSpeedup(item.speedup)}</span>
    `;

    const group = document.createElement("div");
    group.className = "bar-group";
    group.appendChild(createBar("new/delete", item.baselineMs, maxValue, "baseline"));
    group.appendChild(createBar("memory pool", item.poolMs, maxValue, "optimized"));

    card.append(title, group);
    chartArea.appendChild(card);
  }
}

function averageBy(items, key) {
  const groups = new Map();
  for (const item of items) {
    const groupKey = item[key];
    if (!groups.has(groupKey)) {
      groups.set(groupKey, {
        key: groupKey,
        baselineMs: 0,
        poolMs: 0,
        speedup: 0,
        count: 0
      });
    }
    const group = groups.get(groupKey);
    group.baselineMs += item.baselineMs;
    group.poolMs += item.poolMs;
    group.speedup += item.speedup;
    group.count += 1;
  }

  return [...groups.values()]
    .map((group) => ({
      key: group.key,
      baselineMs: group.baselineMs / group.count,
      poolMs: group.poolMs / group.count,
      speedup: group.speedup / group.count
    }))
    .sort((a, b) => a.key - b.key);
}

function buildLinePath(points, xScale, yScale) {
  return points
    .map((point, index) => {
      const x = xScale(point.x);
      const y = yScale(point.y);
      return `${index === 0 ? "M" : "L"} ${x.toFixed(2)} ${y.toFixed(2)}`;
    })
    .join(" ");
}

function niceTickCount(values) {
  if (values.length <= 8) return values;
  const step = Math.ceil(values.length / 7);
  return values.filter((_, index) => index % step === 0 || index === values.length - 1);
}

function renderOriginChart(containerId, config) {
  const container = document.getElementById(containerId);
  if (!container) return;

  const width = 520;
  const height = 300;
  const margin = { top: 20, right: 20, bottom: 46, left: 54 };
  const plotWidth = width - margin.left - margin.right;
  const plotHeight = height - margin.top - margin.bottom;

  const series = config.series;
  const allX = series.flatMap((item) => item.data.map((point) => point.x));
  const allY = series.flatMap((item) => item.data.map((point) => point.y));
  const minX = Math.min(...allX);
  const maxX = Math.max(...allX);
  const maxY = Math.max(...allY, 1);

  const xScale = (value) =>
    margin.left + ((value - minX) / Math.max(1, maxX - minX)) * plotWidth;
  const yScale = (value) => margin.top + plotHeight - (value / maxY) * plotHeight;

  const yTicks = 5;
  const xTicks = niceTickCount([...new Set(allX)]);
  const gridY = Array.from({ length: yTicks + 1 }, (_, i) => (maxY / yTicks) * i);

  const gridMarkup = gridY
    .map((tick) => {
      const y = yScale(tick);
      return `
        <line class="grid-line" x1="${margin.left}" y1="${y}" x2="${width - margin.right}" y2="${y}"></line>
        <text class="tick-label" x="${margin.left - 10}" y="${y + 4}" text-anchor="end">${tick.toFixed(1)}</text>
      `;
    })
    .join("");

  const xTickMarkup = xTicks
    .map((tick) => {
      const x = xScale(tick);
      const rotate = xTicks.length > 6 ? -28 : 0;
      return `<text class="tick-label" x="${x}" y="${height - margin.bottom + 24}" text-anchor="middle" transform="rotate(${rotate} ${x} ${height - margin.bottom + 24})">${tick}</text>`;
    })
    .join("");

  const seriesMarkup = series
    .map((item, seriesIndex) => {
      const path = buildLinePath(item.data, xScale, yScale);
      const points = item.data
        .map((point, pointIndex) => {
          const x = xScale(point.x);
          const y = yScale(point.y);
          return `
            <circle class="origin-point" cx="${x}" cy="${y}" r="4" fill="${item.color}" style="animation-delay:${pointIndex * 0.04}s"></circle>
            <circle class="origin-hover" cx="${x}" cy="${y}" r="10" data-label="${point.x}" data-value="${point.y.toFixed(2)}" data-series="${item.label || ("series-" + seriesIndex)}"></circle>
          `;
        })
        .join("");
      return `<path class="origin-line animate" d="${path}" stroke="${item.color}"></path>${points}`;
    })
    .join("");

  container.innerHTML = `
    <div class="origin-tooltip" id="${containerId}-tooltip"></div>
    <svg class="origin-svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${config.title}">
      <line class="axis-line" x1="${margin.left}" y1="${margin.top}" x2="${margin.left}" y2="${height - margin.bottom}"></line>
      <line class="axis-line" x1="${margin.left}" y1="${height - margin.bottom}" x2="${width - margin.right}" y2="${height - margin.bottom}"></line>
      ${gridMarkup}
      ${seriesMarkup}
      ${xTickMarkup}
      <text class="axis-label" x="${width / 2}" y="${height - 8}" text-anchor="middle">${config.xLabel}</text>
      <text class="axis-label" x="16" y="${height / 2}" text-anchor="middle" transform="rotate(-90 16 ${height / 2})">${config.yLabel}</text>
    </svg>
  `;

  const svg = container.querySelector("svg");
  svg.querySelectorAll(".origin-line").forEach((path) => {
    const length = path.getTotalLength();
    path.style.setProperty("--path-length", `${length}`);
  });

  const tooltip = container.querySelector(".origin-tooltip");
  svg.querySelectorAll(".origin-hover").forEach((target) => {
    target.addEventListener("mouseenter", () => {
      const box = svg.getBoundingClientRect();
      const x = Number(target.getAttribute("cx"));
      const y = Number(target.getAttribute("cy"));
      const left = (x / width) * box.width;
      const top = (y / height) * box.height;
      tooltip.innerHTML = `${target.dataset.series}<br>${config.xLabel}: ${target.dataset.label}<br>${config.yLabel}: ${target.dataset.value}`;
      tooltip.style.left = `${left}px`;
      tooltip.style.top = `${top}px`;
      tooltip.classList.add("visible");
    });
    target.addEventListener("mouseleave", () => {
      tooltip.classList.remove("visible");
    });
  });
}

function renderOriginCharts(report) {
  const comparisons = report.comparisons || [];
  const byThreads = averageBy(comparisons, "threads");
  const bySize = averageBy(comparisons, "allocSize");

  renderOriginChart("thread-line-chart", {
    title: "平均耗时 vs 线程数",
    xLabel: "线程数",
    yLabel: "平均耗时 (ms)",
    series: [
      {
        color: "#cb5f2d",
        label: "new/delete",
        data: byThreads.map((item) => ({ x: item.key, y: item.baselineMs }))
      },
      {
        color: "#0f766e",
        label: "memory pool",
        data: byThreads.map((item) => ({ x: item.key, y: item.poolMs }))
      }
    ]
  });

  renderOriginChart("size-line-chart", {
    title: "平均加速比 vs 对象大小",
    xLabel: "对象大小 (Bytes)",
    yLabel: "平均加速比 (x)",
    series: [
      {
        color: "#4b56d2",
        label: "speedup",
        data: bySize.map((item) => ({ x: item.key, y: item.speedup }))
      }
    ]
  });
}

function groupedComparisons(report, key) {
  const groups = new Map();
  for (const item of report.comparisons || []) {
    const groupKey = item[key];
    if (!groups.has(groupKey)) groups.set(groupKey, []);
    groups.get(groupKey).push(item);
  }
  return [...groups.entries()]
    .map(([groupKey, items]) => ({
      groupKey,
      items: items.sort((a, b) =>
        key === "threads" ? a.allocSize - b.allocSize : a.threads - b.threads
      )
    }))
    .sort((a, b) => a.groupKey - b.groupKey);
}

function createGroupItem(title, items, labelBuilder) {
  const maxValue = Math.max(...items.flatMap((item) => [item.baselineMs, item.poolMs]), 1);
  const preview = items.slice(0, 4);
  const rest = items.slice(4);

  const renderRows = (rows) =>
    rows
      .map(
        (item) => `
          <div class="group-row">
            <span>${labelBuilder(item)}</span>
            <div class="mini-track">
              <div class="mini-fill baseline" style="width:${(item.baselineMs / maxValue) * 100}%"></div>
              <div class="mini-fill optimized" style="width:${(item.poolMs / maxValue) * 100}%"></div>
            </div>
            <strong>${formatSpeedup(item.speedup)}</strong>
          </div>
        `
      )
      .join("");

  return `
    <article class="group-item">
      <h4>${title}</h4>
      <div class="group-preview">${renderRows(preview)}</div>
      ${
        rest.length
          ? `<details><summary>展开剩余 ${rest.length} 项</summary><div class="group-full">${renderRows(rest)}</div></details>`
          : ""
      }
    </article>
  `;
}

function renderGroupedPanels(report) {
  const threadHost = document.getElementById("thread-group-list");
  const sizeHost = document.getElementById("size-group-list");
  if (!threadHost || !sizeHost) return;

  const threadGroups = groupedComparisons(report, "threads");
  const sizeGroups = groupedComparisons(report, "allocSize");

  threadHost.innerHTML = threadGroups
    .map((group) =>
      createGroupItem(`${group.groupKey} 线程`, group.items, (item) => `${item.allocSize}B`)
    )
    .join("");

  sizeHost.innerHTML = sizeGroups
    .map((group) =>
      createGroupItem(`${group.groupKey} Bytes`, group.items, (item) => `${item.threads}T`)
    )
    .join("");
}

function renderTable(report) {
  const body = document.getElementById("comparison-body");
  body.innerHTML = "";

  const sorted = [...(report.comparisons || [])].sort((a, b) => {
    if (b.speedup !== a.speedup) {
      return b.speedup - a.speedup;
    }
    if (a.threads !== b.threads) {
      return a.threads - b.threads;
    }
    return a.allocSize - b.allocSize;
  });

  for (const item of sorted) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${item.threads}</td>
      <td>${item.allocSize} B</td>
      <td>${item.iterations.toLocaleString()}</td>
      <td>${item.baselineMs} ms</td>
      <td>${item.poolMs} ms</td>
      <td>${formatSpeedup(item.speedup)}</td>
    `;
    body.appendChild(tr);
  }
}

function renderFallback(error) {
  document.getElementById("chart-area").innerHTML = `
    <div class="chart-card">
      <div class="chart-title">
        <h4>报告未加载</h4>
        <span>请先运行 memory_pool_demo.exe</span>
      </div>
      <p class="section-desc">
        当前页面没有读到 <code>ui/data/benchmark_report.json</code>。错误信息：${error.message}
      </p>
    </div>
  `;
}

loadReport()
  .then((report) => {
    renderSummary(report);
    renderTable(report);
    renderOriginCharts(report);
    renderGroupedPanels(report);
    renderCharts(report);
  })
  .catch((error) => {
    renderFallback(error);
  });
