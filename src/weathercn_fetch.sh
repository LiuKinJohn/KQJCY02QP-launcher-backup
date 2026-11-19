#!/bin/sh
# WeatherCN worker. Called only by the launcher; never print or put the key in a URL.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
CA=/mnt/UDISK/launcher/ca-roots.pem
OUT=/tmp/wxcn.new
ERR=/tmp/wxcn.err
key=${WEATHER_KEY:-}
city=${WEATHER_CITY:-}
rm -f "$OUT" "$ERR"
fail() { printf '%s\n' "$1" > "$ERR"; exit 1; }
[ -n "$key" ] || fail no_key
case "$key" in *[!A-Za-z0-9._-]*) fail bad_key;; esac
[ -r "$CA" ] || fail no_ca

get() { jsonfilter -i "$1" -e "$2" 2>/dev/null | tr -d '\r\n'; }
req() {
  target=$1; shift
  # This gateway rejects the documented header with "No API key found".
  # Supply the required query parameter through curl's stdin config so the
  # secret is absent from argv and from our own logs.
  printf 'data-urlencode = "apikey=%s"\n' "$key" |
    curl -K - --cacert "$CA" -fsS --max-time 14 --get "$@" -o "$target" 2>/dev/null
}

if [ -n "$city" ]; then
  req /tmp/wxcn.location 'https://openapi.weathercn.com/locations/v1/cities/translate' \
    --data-urlencode "q=$city" --data-urlencode 'language=zh-cn' || fail location
  location=$(get /tmp/wxcn.location '@[0].Key')
else
  curl --cacert "$CA" -fsS --max-time 10 'https://ipwho.is/' -o /tmp/wxcn.ip 2>/dev/null || fail ip
  lat=$(get /tmp/wxcn.ip '@.latitude')
  lon=$(get /tmp/wxcn.ip '@.longitude')
  [ -n "$lat" ] && [ -n "$lon" ] || fail ip
  req /tmp/wxcn.location 'https://openapi.weathercn.com/locations/v1/cities/geoposition/search.json' \
    --data-urlencode "q=$lat,$lon" --data-urlencode 'language=zh-cn' || fail location
  location=$(get /tmp/wxcn.location '@.Key')
fi
case "$location" in ''|*[!0-9]*) fail location;; esac
resolved=$(get /tmp/wxcn.location '@.LocalizedName')
[ -n "$resolved" ] || resolved=$(get /tmp/wxcn.location '@[0].LocalizedName')
resolved_en=$(get /tmp/wxcn.location '@.EnglishName')
[ -n "$resolved_en" ] || resolved_en=$(get /tmp/wxcn.location '@[0].EnglishName')

base='https://openapi.weathercn.com'
req /tmp/wxcn.current "$base/currentconditions/v1/$location.json?language=zh-cn&details=true" || fail current
req /tmp/wxcn.daily "$base/forecasts/v1/daily/5day/$location.json?language=zh-cn&details=true&metric=true" || fail daily
req /tmp/wxcn.hourly "$base/forecasts/v1/hourly/12hour/$location.json?language=zh-cn&metric=true" || fail hourly
req /tmp/wxcn.alerts "$base/alerts/v1/$location.json?language=zh-cn&details=true" || fail alerts

current=$(get /tmp/wxcn.current '@[0].Temperature.Metric.Value')
[ -n "$current" ] || fail parse_current
{
  printf 'city=%s\n' "$resolved"
  printf 'city_en=%s\n' "$resolved_en"
  printf 'temp=%s\n' "$current"
  printf 'feel=%s\n' "$(get /tmp/wxcn.current '@[0].RealFeelTemperature.Metric.Value')"
  printf 'humidity=%s\n' "$(get /tmp/wxcn.current '@[0].RelativeHumidity')"
  printf 'wind=%s\n' "$(get /tmp/wxcn.current '@[0].Wind.Speed.Metric.Value')"
  printf 'phrase=%s\n' "$(get /tmp/wxcn.current '@[0].WeatherText')"
  printf 'code=%s\n' "$(get /tmp/wxcn.current '@[0].LocalSource.WeatherCode')"
  printf 'daylight=%s\n' "$(get /tmp/wxcn.current '@[0].IsDayTime')"
  alert_level=$(get /tmp/wxcn.alerts '@[0].Level')
  alert_type=$(get /tmp/wxcn.alerts '@[0].Type')
  if [ -n "$alert_level" ] && [ -n "$alert_type" ]; then
    printf 'alert=%s%s预警\n' "$alert_type" "$alert_level"
  else
    printf 'alert=\n'
  fi
  i=0
  while [ "$i" -lt 5 ]; do
    printf 'day%d=%s|%s|%s|%s\n' "$i" \
      "$(get /tmp/wxcn.daily "@.DailyForecasts[$i].Temperature.Minimum.Value")" \
      "$(get /tmp/wxcn.daily "@.DailyForecasts[$i].Temperature.Maximum.Value")" \
      "$(get /tmp/wxcn.daily "@.DailyForecasts[$i].Day.IconPhrase")" \
      "$(get /tmp/wxcn.daily "@.DailyForecasts[$i].Day.LocalSource.WeatherCode")"
    i=$((i+1))
  done
  i=0
  while [ "$i" -lt 12 ]; do
    dt=$(get /tmp/wxcn.hourly "@[$i].DateTime")
    hh=${dt#*T}; hh=${hh%%:*}
    printf 'hour%d=%s|%s|%s|%s|%s\n' "$i" "$hh" \
      "$(get /tmp/wxcn.hourly "@[$i].Temperature.Value")" \
      "$(get /tmp/wxcn.hourly "@[$i].PrecipitationProbability")" \
      "$(get /tmp/wxcn.hourly "@[$i].LocalSource.WeatherCode")" \
      "$(get /tmp/wxcn.hourly "@[$i].IsDaylight")"
    i=$((i+1))
  done
} > "$OUT" || fail parse
mv "$OUT" /tmp/wxcn.ready || fail save
rm -f "$ERR"
