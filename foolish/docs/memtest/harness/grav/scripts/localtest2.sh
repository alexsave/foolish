#!/bin/bash
# localtest2.sh: the capture, sampler and tlsres path again after the bg() change.
mkdir -p /opt/mt; cp /g/pkg/* /opt/mt/; cp /g/bin/cbase_D /opt/mt/; echo none > /opt/mt/bucket; chmod +x /opt/mt/*.sh /opt/mt/cbase_D
dnf install -y procps-ng openssl > /dev/null 2>&1
bash /opt/mt/load.sh build 2>&1 | tail -1
bash /opt/mt/srv.sh start t1 D 8080 6
bash /opt/mt/srv.sh start t1b D 8081
sleep 1; bash /opt/mt/srv.sh idle t1b_idle
bash /opt/mt/load.sh run t1 127.0.0.1 4 0 1 100000 8000 | head -1
cat /var/tmp/mt/capout_t1_mid.txt; cat /var/tmp/mt/tls_t1_mid.txt; wc -l /var/tmp/mt/rss_t1.log; tail -2 /var/tmp/mt/rss_t1.log; cat /var/tmp/mt/pid_t1 /var/tmp/mt/pid_t1b
bash /opt/mt/srv.sh stop t1; bash /opt/mt/srv.sh stop t1b
