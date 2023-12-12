package metrics

func RegisterForProxy() {
	RegisterProxy(collectProxyMetrics)
	RegisterProxy(collectPorxyCmdMetrics)
}

var collectProxyMetrics map[string]MetricConfig = map[string]MetricConfig{
	"ops_total": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "ops_total",
			Help:      "proxy total ops",
			Type:      metricTypeCounter,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "ops_total",
		},
	},
	"ops_fails": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "ops_fails",
			Help:      "proxy fails counter",
			Type:      metricTypeCounter,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "ops_fails",
		},
	},
	"qps": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "qps",
			Help:      "The Proxy qps",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "ops_qps",
		},
	},
	"rusage_cpu": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "rusage_cpu",
			Help:      "The CPU usage rate of the proxy",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "rusage_cpu",
		},
	},
	"reusage_mem": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "rusage_mem",
			Help:      "The mem usage of the proxy",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "rusage_mem",
		},
	},
	"online": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "online",
			Help:      "Is the Proxy online",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "online",
		},
	},
	"timeout_cmd_number": {
		Parser: &normalParser{},
		MetricMeta: &MetaData{
			Name:      "timeout_cmd_number",
			Help:      "The number of commands recorded in the slow log within the last second",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName},
			ValueName: "timeout_cmd_number",
		},
	},
}

var collectPorxyCmdMetrics map[string]MetricConfig = map[string]MetricConfig{
	"calls": {
		Parser: &proxyParser{},
		MetricMeta: &MetaData{
			Name:      "calls",
			Help:      "the number of cmd calls",
			Type:      metricTypeCounter,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName, LabelOpstr},
			ValueName: "calls",
		},
	},
	"usecs_percall": {
		Parser: &proxyParser{},
		MetricMeta: &MetaData{
			Name:      "usecs_percall",
			Help:      "Average duration per call",
			Type:      metricTypeGauge,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName, LabelOpstr},
			ValueName: "usecs_percall",
		},
	},
	"fails": {
		Parser: &proxyParser{},
		MetricMeta: &MetaData{
			Name:      "fails",
			Help:      "the number of cmd fail",
			Type:      metricTypeCounter,
			Labels:    []string{LabelNameAddr, LabelID, LabelProductName, LabelOpstr},
			ValueName: "fails",
		},
	},
}
