#!/bin/bash
# localtest.sh: inside an amazonlinux:2023 container, the grav pkg exercised end to end before AWS: build the clients,
# start cbase_D, a short sat2, a rematch run (2 s and 65 s waits), and tlsres on the server.
set -x
mkdir -p /opt/mt; cp /g/pkg/* /opt/mt/; cp /g/bin/cbase_D /opt/mt/; echo none > /opt/mt/bucket; chmod +x /opt/mt/*.sh /opt/mt/cbase_D
dnf install -y procps-ng openssl > /dev/null 2>&1
bash /opt/mt/load.sh build 2>&1 | tail -3
bash /opt/mt/srv.sh start t1 D 8080 12
bash /opt/mt/load.sh run t1 127.0.0.1 4 0 1 100000 8000
cat /var/tmp/mt/capout_t1_mid.txt; head -20 /var/tmp/mt/tls_t1_mid.txt
bash /opt/mt/srv.sh stop t1
bash /opt/mt/srv.sh start t2 D 8080
bash /opt/mt/load.sh rematch t2 127.0.0.1 2 2000 3
bash /opt/mt/srv.sh stop t2
bash /opt/mt/srv.sh start t3 D 8080
bash /opt/mt/load.sh rematch t3 127.0.0.1 2 65000 2
bash /opt/mt/srv.sh stop t3
grep -h "STALL\|failed\|refused" /var/tmp/mt/t*/*.err | head
