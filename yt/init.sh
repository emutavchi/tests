#!/bin/sh

crontab -r -c /var/spool/cron/
crontab -l -c /var/spool/cron/
systemctl stop logrotate.service logrotate.timer
systemctl status logrotate.service logrotate.timer

export XDG_RUNTIME_DIR=/run
westeros-gl-console set auto-frm-mode 0
westeros-gl-console set mode 3840x2160px60

TOKEN=`WPEFrameworkSecurityUtility 2>/dev/null | cut -f 4 -d '"'`
curl -v -H "Authorization: Bearer ${TOKEN}" 'http://127.0.0.1:9998/jsonrpc' -d '{"method":"Controller.1.deactivate","params":{"callsign":"HtmlApp-0"}}'; echo
curl -v -H "Authorization: Bearer ${TOKEN}" 'http://127.0.0.1:9998/jsonrpc' -d '{"method":"Controller.1.deactivate","params":{"callsign":"Netflix-0"}}'; echo
curl -v -H "Authorization: Bearer ${TOKEN}" 'http://127.0.0.1:9998/jsonrpc' -d '{"method":"Controller.1.status"}' | json_reformat; echo

