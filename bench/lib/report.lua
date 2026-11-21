-- wrk reporter: prints one machine-readable JSON line after the human report.
-- Latencies are in microseconds. Used by bench/run.sh; works with any wrk run:
--   wrk -t4 -c256 -d15s --latency -s bench/lib/report.lua http://127.0.0.1:18080/

-- Optional extra header from the environment, e.g. WRK_HEADER="Connection: close".
local extra = os.getenv("WRK_HEADER")
if extra and extra ~= "" then
  local k, v = extra:match("^%s*([^:]+):%s*(.-)%s*$")
  if k then wrk.headers[k] = v end
end

done = function(summary, latency, requests)
  local secs = summary.duration / 1e6
  local e = summary.errors
  local http_err = summary.errors.status  -- responses with status >= 400
  io.write(string.format(
    'JSON:{"requests":%d,"duration_s":%.3f,"rps":%.1f,"bytes":%d,' ..
    '"lat_mean_us":%.1f,"lat_stdev_us":%.1f,"lat_max_us":%d,' ..
    '"p50_us":%d,"p75_us":%d,"p90_us":%d,"p99_us":%d,"p999_us":%d,' ..
    '"err_connect":%d,"err_read":%d,"err_write":%d,"err_timeout":%d,"http_errors":%d}\n',
    summary.requests, secs, summary.requests / secs, summary.bytes,
    latency.mean, latency.stdev, latency.max,
    latency:percentile(50), latency:percentile(75), latency:percentile(90),
    latency:percentile(99), latency:percentile(99.9),
    e.connect, e.read, e.write, e.timeout, http_err))
end
