#ifndef UI_I18N_H
#define UI_I18N_H

/* Chinese remains the default. Keep translations at the painter boundary so
   every existing screen, including sensor tags and weather phrases, uses the
   same setting. Dynamic values retain their numeric content. */
static int ui_lang; /* 0 zh-CN, 1 en */

struct ui_pair { const char *zh, *en; };
static const struct ui_pair ui_pairs[] = {
    {"室内空气", "Indoor air"}, {"温度", "Temperature"}, {"湿度", "Humidity"},
    {"空气质量", "Air quality"}, {"光照", "Light"}, {"天气", "Weather"},
    {"时钟", "Clock"}, {"设置", "Settings"}, {"计算器", "Calculator"},
    {"日历", "Calendar"}, {"便签", "Notes"}, {"文件", "Files"},
    {"添加", "Add"}, {"编辑", "Edit"}, {"删除", "Delete"},
    {"开发中", "Coming soon"}, {"轻点切换样式", "Tap to change style"},
    {"时钟设置", "Clock settings"}, {"大字", "Large"}, {"渐变", "Gradient"},
    {"样式", "Style"}, {"雅黑", "YaHei"}, {"细", "Light"},
    {"常规", "Regular"}, {"中等", "Medium"}, {"粗", "Bold"},
    {"字号", "Size"}, {"字体", "Font"}, {"字重", "Weight"},
    {"更换主题", "Change theme"}, {"壁纸明暗", "Wallpaper tone"},
    {"关于本机", "About device"}, {"选择主题", "Choose theme"},
    {"点任意位置关闭", "Tap anywhere to close"},
    {"米家空气检测仪 KQJCY02QP", "Mi Air Monitor KQJCY02QP"},
    {"自建 Launcher 1.0", "Custom Launcher 1.0"},
    {"界面", "Interface"}, {"运行", "Runtime"},

    {"网络", "Network"}, {"主题", "Theme"}, {"浅色", "Light"},
    {"主题 / 字体", "Theme / font"},
    {"深色", "Dark"}, {"设备状态", "Device status"},
    {"显示与刷新", "Display & refresh"}, {"天气设置", "Weather settings"},
    {"传感器", "Sensors"},
    {"传感器 / 电源", "Sensors / power"}, {"亮度 / 息屏", "Brightness / sleep"},
    {"电源", "Power"}, {"网络状态", "Network status"},
    {"传感器状态", "Sensors"}, {"已保存", "Saved"},
    {"保存中…", "Saving…"}, {"保存失败", "Save failed"},
    {"语言", "Language"}, {"简体中文", "简体中文"},
    {"型号", "Model"}, {"系统", "System"}, {"运行时间", "Uptime"},
    {"屏幕亮度", "Brightness"}, {"闲置后调暗", "Dim after idle"},
    {"调暗亮度", "Dim level"}, {"天气刷新", "Weather refresh"},
    {"时间", "Time"}, {"UTC 时差", "Time zone"},
    {"UTC / 网络校时", "Zone / online sync"},
    {"当前时间", "Current time"}, {"网络校时", "Sync now"},
    {"自动间隔", "Auto interval"},
    {"备用时间源", "Backup source"},
    {"上次校时", "Last sync"}, {"轻点校时", "Tap to sync"},
    {"校时中…", "Syncing…"}, {"校时失败 · 重试", "Failed · retry"},
    {"已校时 · 重校", "Synced · resync"},
    {"请连接 Wi-Fi", "Connect Wi-Fi"},
    {"关闭", "Off"}, {"低", "Low"}, {"中", "Medium"},
    {"定位", "Location"}, {"城市", "City"},
    {"手动城市", "Manual city"}, {"自动 / IP", "Auto / IP"},
    {"自动定位", "Auto location"}, {"轻点输入", "Tap to enter"},
    {"华风爱科 API Key", "WeatherCN API key"},
    {"已设置", "Set"}, {"未设置", "Not set"},
    {"刷新间隔", "Refresh interval"}, {"数据源", "Source"},
    {"华风爱科", "WeatherCN"},

    {"供电来源", "Power source"}, {"电池电量", "Battery level"},
    {"充电状态", "Charge state"}, {"电能流向", "Power flow"},
    {"电池侧功率估算", "Battery power est."}, {"电压 · 电流", "Voltage · current"},
    {"电池", "Battery"}, {"电池供电", "On battery"},
    {"USB 供电", "USB power"},
    {"充电器", "Charger"},
    {"电量低", "Low battery"}, {"电压偏低", "Low voltage"},
    {"电池 / 电压偏低", "Battery / low voltage"},
    {"充电", "Charging"}, {"充电中", "Charging"},
    {"已充满", "Full"}, {"已满", "Full"}, {"未充电", "Not charging"},
    {"USB 接入 / 未充电", "USB connected / battery idle"},
    {"外接电源 / 未充电", "External power / idle"},
    {"待确认", "Checking"}, {"状态待确认", "State unclear"},
    {"USB 输入 / 电池充电", "USB in / charging"},
    {"USB 输入 / 电池已满", "USB in / battery full"},
    {"USB 输入 / 电池待机", "USB in / battery idle"},
    {"电源输入 / 电池充电", "Power in / charging"},
    {"电源输入 / 电池已满", "Power in / battery full"},
    {"电源输入 / 电池待机", "Power in / battery idle"},
    {"电池输出 / 设备运行", "Battery out / running"},
    {"连接状态", "Connection"}, {"网络名称", "Network name"},
    {"IPv4 地址", "IPv4 address"}, {"无线接口", "Wi-Fi interface"},
    {"天气数据", "Weather data"}, {"获取状态", "Fetch state"},
    {"已连接", "Connected"}, {"未连接", "Disconnected"},
    {"获取中", "Fetching"}, {"已更新", "Updated"},
    {"获取失败", "Fetch failed"}, {"等待网络", "Waiting for network"},

    {"扫描附近 WiFi", "Scan Wi-Fi"}, {"扫描中…", "Scanning…"},
    {"扫描失败 · 重试", "Scan failed · retry"},
    {"连接中…", "Connecting…"},
    {"连接失败，请重试", "Connection failed · retry"},
    {"输入", "Enter text"}, {"密码", "Password"},
    {"等待天气数据", "Waiting for weather"},
    {"正在获取华风爱科天气…", "Fetching WeatherCN…"},
    {"天气获取失败", "Weather fetch failed"},
    {"IP 定位失败，可在设置中填写城市", "IP location failed; set a city"},
    {"城市解析失败，请检查城市名称", "City not found; check its name"},
    {"证书文件缺失", "CA certificate missing"},
    {"请检查 API Key", "Check API key"},
    {"接口请求失败，请检查密钥与网络", "Check API key and network"},
    {"位置会按网络 IP 粗略定位", "Approximate location from IP"},
    {"获取中…", "Fetching…"}, {"重试", "Retry"}, {"刷新", "Refresh"},
    {"现在", "Now"}, {"5日", "5 days"}, {"12小时", "12 hours"},
    {"生效预警", "Active alert"}, {"暂无生效预警", "No active alerts"},
    {"今天", "Today"}, {"明天", "Tomorrow"}, {"后天", "Day 3"},
    {"第4天", "Day 4"}, {"第5天", "Day 5"},
    {"天气 · 北京 (Open-Meteo)", "Weather · Beijing (Open-Meteo)"},
    {"天气信息", "Weather info"}, {"天气预报", "Forecast"},
    {"尚未获取", "No data yet"}, {"轻点刷新重试", "Tap refresh to retry"},
    {"正在获取…", "Fetching…"},

    {"晴", "Sunny"}, {"少云", "Mostly sunny"}, {"多云", "Cloudy"},
    {"云", "Cloud"},
    {"阴", "Overcast"}, {"雷", "Thunder"}, {"雷雨", "Storms"},
    {"雨", "Rain"}, {"阵雨", "Showers"}, {"毛毛雨", "Drizzle"},
    {"冻雨", "Freezing rain"}, {"雪", "Snow"}, {"阵雪", "Snow showers"},
    {"雾", "Fog"}, {"霾", "Haze"},
    {"小雨", "Light rain"}, {"中雨", "Rain"}, {"大雨", "Heavy rain"},
    {"雷阵雨", "Storms"}, {"雨夹雪", "Sleet"},
    {"小雪", "Light snow"}, {"中雪", "Snow"}, {"大雪", "Heavy snow"},
    {"多云转晴", "Clearing"}, {"间歇性阵雨", "Scattered showers"},
    {"雷暴", "Thunderstorm"}, {"局部多云", "Partly cloudy"},
    {"部分晴朗", "Partly sunny"},

    {"当前读数", "Current"}, {"来源", "Source"},
    {"原始（含板热）", "Raw (board heat)"},
    {"原始（未修正）", "Raw (uncorrected)"},
    {"已补偿", "Compensated"}, {"补偿量", "Adjustment"},
    {"质量等级", "AQI level"}, {"主导污染物", "Main pollutant"},
    {"PM2.5 当前", "PM2.5 now"}, {"依据", "Method"},
    {"1分钟均值按24h档位估算", "1-min mean, 24h AQI scale"},
    {"基线学习", "Baseline learning"}, {"基线值", "Baseline"},
    {"开始基线学习", "Start baseline learning"},
    {"重新学习基线", "Relearn baseline"},
    {"学习中", "Learning"}, {"正在保存", "Saving"},
    {"基线已保存", "Baseline saved"}, {"已保存基线", "Baseline saved"},
    {"未学习", "Not learned"},
    {"保存并校验中…", "Saving and verifying…"},
    {"请保持环境安静，不要触碰设备", "Keep air steady; do not touch"},
    {"正在确认基线已写入设备存储", "Verifying baseline in storage"},
    {"轻点可重新学习基线", "Tap to relearn baseline"},
    {"请检查传感器后重试", "Check sensor and retry"},
    {"需要静置 20 分钟", "Leave undisturbed for 20 min"},
    {"基线保存超时", "Baseline save timed out"},
    {"基线写入失败", "Baseline write failed"},
    {"基线值无效", "Invalid baseline"},
    {"无法启动保存", "Could not save"},
    {"UDISK 未挂载", "Storage unavailable"},
    {"请求超时", "Request timed out"}, {"数据格式异常", "Bad data format"},
    {"需要先连上 WiFi", "Connect Wi-Fi first"},

    {"优", "Good"}, {"良", "Fair"}, {"良好", "Good"},
    {"中等", "Moderate"}, {"偏中", "Moderate"},
    {"轻度污染", "Sensitive groups"},
    {"中度污染", "Unhealthy"}, {"重度污染", "Very unhealthy"},
    {"严重污染", "Hazardous"}, {"超量程", "Over range"},
    {"偏高", "High"}, {"偏低", "Low"}, {"偏湿", "Humid"},
    {"偏干", "Dry"}, {"偏热", "Warm"}, {"偏凉", "Cool"},
    {"舒适", "OK"}, {"明亮", "Bright"},
    {"适中", "Moderate"}, {"暗", "Dim"},
    {"无响应", "No response"}, {"暂无数据", "No data"},
    {"正常", "Normal"}, {"异常", "Abnormal"},
    {"光感 I2C 1-0023", "Light sensor I2C 1-0023"},
    {"串口粒子计数（ttyS3）", "Particle sensor (ttyS3)"},
};

static const char *ui_tr(const char *s)
{
    if (!ui_lang || !s || !*s) return s;
    for (size_t i = 0; i < sizeof ui_pairs / sizeof ui_pairs[0]; i++)
        if (!strcmp(s, ui_pairs[i].zh)) return ui_pairs[i].en;

    /* Short suffixes and compound values produced by snprintf. */
    static const struct ui_pair fragments[] = {
        {"已完成 · ", "Done · "}, {"失败 · ", "Failed · "},
        {"剩余 ", "Remaining "},
        {"已连接 ", "Connected "}, {"分钟前", " min ago"},
        {"小时前", " h ago"}, {"天前", " d ago"},
        {" 分钟", " min"}, {" 小时", " h"}, {" 秒", " s"},
        {" 分", " min"},
        {"体感 ", "Feels "}, {"湿度 ", "Humidity "},
        {"风 ", "Wind "}, {"约 ", "~"},
        {"充电", "Charging"},
        {"已满", "Full"}, {"电池供电", "On battery"},
        {"USB 供电", "USB power"},
        {"电量低", "Low battery"}, {"电压偏低", "Low voltage"},
        {"良好", "Good"}, {"偏高", "High"}, {"偏湿", "Humid"},
        {"舒适", "Comfortable"}, {"无响应", "No response"},
        {"多云", "Cloudy"}, {"雷雨", "Thunderstorm"},
    };
    int has_fragment = 0;
    for (size_t i = 0; i < sizeof fragments / sizeof fragments[0]; i++)
        if (strstr(s, fragments[i].zh)) { has_fragment = 1; break; }
    if (!has_fragment) return s;
    static char out[4][256];
    static unsigned next;
    char *dst = out[next++ % 4];
    snprintf(dst, 256, "%s", s);
    for (size_t i = 0; i < sizeof fragments / sizeof fragments[0]; i++) {
        const char *from = fragments[i].zh, *to = fragments[i].en;
        char *at = strstr(dst, from);
        if (!at) continue;
        char temp[256];
        size_t left = (size_t)(at - dst);
        snprintf(temp, sizeof temp, "%.*s%s%s", (int)left, dst, to, at + strlen(from));
        snprintf(dst, 256, "%s", temp);
    }
    return dst;
}

#endif
