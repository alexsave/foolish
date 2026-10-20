#!/usr/bin/env python3
# srvctl.py : the server box's control daemon on [::]:9000 (reachable only inside the temp security group).
# The load box's master.sh drives it; the requester's address is the load box, which is also the peer whose
# neighbour MAC the lwIP backend writes into its Ethernet headers.
#   /ping                         pong (and BUILD_OK or BUILD_FAILED)
#   /facts                        kernel, ENA driver, queues, coalescing, offloads, kernel config, IRQs
#   /nic?cfg=prod                 detach XDP, restore the original MTU and queue count
#   /nic?cfg=xdp                  first call: the attach ladder (driver mode at the default config, then a lower MTU,
#                                 then one queue, then MTU 1500, then generic mode), recording every attempt and dmesg;
#                                 later calls re-apply what worked
#   /start?backend=kern|xdp&spin=US&label=L&extra=ARGS   start xsrv, wait for READY and for the network to answer
#   /stop?label=L                 SIGTERM the server, return its log tail (the FINAL snapshot)
#   /check                        coexistence: IMDS, DNS, S3, the SSM agent and its endpoint, sshd
#   /poweroff
import http.server, socketserver, socket, subprocess, os, signal, time, urllib.parse, resource, re, shlex

D = '/var/tmp/xs'
BIN = D + '/bin'
LOG = D + '/srvctl.log'
ST = {'srv': None, 'label': None, 'out': None, 'xdp': None, 'ladder': None}


def say(s):
    with open(LOG, 'a') as f:
        f.write('%s %s\n' % (time.strftime('%H:%M:%S'), s))


def run(cmd, t=90):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return 124, 'timeout\n'


def sh(cmd, t=90):
    rc, o = run(cmd, t)
    return '$ %s\n%s[rc %d]\n' % (cmd, o, rc)


def ifname():
    rc, o = run('ip -6 route show default')
    m = re.search(r'dev (\S+)', o)
    return m.group(1) if m else 'ens5'


IF = ifname()


def mtu():
    return int(open('/sys/class/net/%s/mtu' % IF).read())


def channels():
    rc, o = run('ethtool -l %s' % IF)
    v = re.findall(r'Combined:\s+(\d+)', o)
    return (int(v[0]), int(v[1])) if len(v) >= 2 else (1, 1)  # (max, current)


ORIG_MTU = mtu()
MAXQ = channels()[0]


def myip6():
    rc, o = run('ip -6 -o addr show dev %s scope global' % IF)
    for l in o.splitlines():
        if 'tentative' in l:
            continue
        m = re.search(r'inet6 ([0-9a-f:]+)/', l)
        if m:
            return m.group(1)
    return None


def wait_net(peer, limit=150):
    t0 = time.time()
    while time.time() - t0 < limit:
        if myip6() and run('ping -6 -c1 -W1 %s' % peer, 5)[0] == 0:
            return round(time.time() - t0, 2)
        time.sleep(0.5)
    return None


def dmesg_lines():
    return run('dmesg')[1].splitlines()


def set_nic(want_mtu, want_q, out):
    cur_max, cur_q = channels()
    if want_q != cur_q:
        out.append(sh('ethtool -L %s combined %d' % (IF, want_q)))
    if want_mtu != mtu():
        out.append(sh('ip link set dev %s mtu %d' % (IF, want_mtu)))


def nic(q, peer):
    cfg = q.get('cfg', 'prod')
    out = []
    d0 = len(dmesg_lines())
    t0 = time.time()
    if cfg == 'prod':
        out.append(sh('%s/xsrv detach --if %s' % (BIN, IF)))
        set_nic(ORIG_MTU, MAXQ, out)
        ST['xdp'] = None
    else:
        ladder = [(ORIG_MTU, MAXQ, 'drv'), (3498, MAXQ, 'drv'), (3498, 1, 'drv'), (1500, 1, 'drv'), (ORIG_MTU, MAXQ, 'skb')]
        if ST['ladder']:
            ladder = [ST['ladder']]
        ok = None
        for (m, nq, mode) in ladder:
            out.append('--- attempt mode=%s mtu=%d queues=%d\n' % (mode, m, nq))
            set_nic(m, nq, out)
            w = wait_net(peer)
            out.append('network back after %s s\n' % w)
            dd = len(dmesg_lines())
            rc, o = run('%s/xsrv attach --if %s --mode %s --obj %s/xdp_prog.o' % (BIN, IF, mode, BIN))
            out.append('$ xsrv attach mode=%s\n%s[rc %d]\n' % (mode, o, rc))
            out.append('dmesg:\n' + '\n'.join(dmesg_lines()[dd:]) + '\n')
            if rc == 0:
                ok = (m, nq, mode)
                break
        if ok:
            ST['ladder'] = ok
            ST['xdp'] = ok
            out.append('XDP_OK mode=%s mtu=%d queues=%d\n' % (ok[2], ok[0], ok[1]))
        else:
            out.append('XDP_FAILED\n')
    w = wait_net(peer)
    out.append('network back after %s s (total %.1f s)\n' % (w, time.time() - t0))
    out.append(sh('ip -d link show %s' % IF) + sh('ethtool -l %s' % IF))
    out.append('dmesg during nic change:\n' + '\n'.join(dmesg_lines()[d0:]) + '\n')
    say('nic %s -> %s' % (cfg, ST['xdp']))
    return ''.join(out)


def facts(q, peer):
    cmds = ['uname -a', 'head -3 /etc/os-release', 'nproc', 'free -m', 'ethtool -i %s' % IF, 'ethtool -l %s' % IF,
            'ethtool -c %s' % IF, 'ethtool -g %s' % IF, "ethtool -k %s | grep -E 'checksum|segmentation|offload|xdp'" % IF,
            'ip -d link show %s' % IF, 'ip -6 addr', 'ip -6 route', 'ip -6 neigh', 'modinfo ena | head -20',
            "grep -E 'CONFIG_XDP_SOCKETS|CONFIG_BPF_JIT=|CONFIG_NET_RX_BUSY_POLL|CONFIG_IRQ_TIME|CONFIG_HZ=|CONFIG_NO_HZ_FULL|CONFIG_VIRT_CPU|CONFIG_ENA' /boot/config-$(uname -r)",
            "grep -iE 'ena|%s|CPU' /proc/interrupts" % IF, 'for i in $(grep -iE "%s|ena" /proc/interrupts | cut -d: -f1); do echo "irq $i -> cpus $(cat /proc/irq/$i/smp_affinity_list)"; done' % IF,
            'systemctl is-active irqbalance', 'ls /sys/kernel/tracing/events/power', 'ls /sys/devices/system/cpu/cpu0/cpuidle 2>&1 | head',
            'ethtool -S %s | head -80' % IF, 'cat /proc/cmdline', 'rpm -q libbpf clang gcc kernel', 'mount | grep -E "bpf|tracefs"']
    return ''.join(sh(c) for c in cmds) + 'ORIG_MTU %d MAXQ %d IF %s\n' % (ORIG_MTU, MAXQ, IF)


def neigh_mac(peer):
    run('ping -6 -c1 -W1 %s' % peer, 5)
    rc, o = run('ip -6 neigh show %s dev %s' % (peer, IF))
    m = re.search(r'lladdr ([0-9a-f:]{17})', o)
    return m.group(1) if m else None


def start(q, peer):
    if ST['srv'] is not None:
        stop({}, peer)
    label = q.get('label', 'x')
    be = q.get('backend', 'kern')
    out = D + '/runs/' + label
    os.makedirs(out, exist_ok=True)
    args = [BIN + '/xsrv', be, '--if', IF, '--port', '7000', '--ctl', '7100', '--spin', q.get('spin', '20')]
    note = ''
    if be == 'xdp':
        mac = neigh_mac(peer)
        args += ['--ip6', myip6()]
        if mac:
            args += ['--gw-mac', mac]
        note = 'peer %s neigh mac %s\n' % (peer, mac)
    args += shlex.split(q.get('extra', ''))
    logf = open(out + '/server.log', 'w')

    def pre():
        resource.setrlimit(resource.RLIMIT_NOFILE, (65536, 65536))
        resource.setrlimit(resource.RLIMIT_MEMLOCK, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
        os.setsid()
    d0 = len(dmesg_lines())
    srv = subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT, preexec_fn=pre, cwd=D)
    ST.update(srv=srv, label=label, out=out)
    ready = False
    for _ in range(200):
        txt = open(out + '/server.log').read()
        if 'READY' in txt:
            ready = True
            break
        if srv.poll() is not None:
            break
        time.sleep(0.1)
    w = wait_net(peer, 90)
    say('start %s %s ready=%s net=%s' % (label, ' '.join(args), ready, w))
    return ('cmd %s\n%snetwork answered after %s s\n' % (' '.join(args), note, w) + open(out + '/server.log').read() +
            'dmesg:\n' + '\n'.join(dmesg_lines()[d0:]) + '\n')


def stop(q, peer):
    srv = ST['srv']
    if srv is None:
        return 'nothing running\n'
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(15)
    except Exception:
        srv.kill()
        srv.wait()
    out = ST['out']
    ST.update(srv=None, label=None, out=None)
    txt = open(out + '/server.log').read().splitlines()
    say('stop rc=%s' % srv.returncode)
    return 'rc %s\n' % srv.returncode + '\n'.join(txt[-40:]) + '\n'


def check(q, peer):
    b = open(D + '/bucket.txt').read().strip()
    tok = "$(curl -s -m 3 -X PUT http://[fd00:ec2::254]/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 60')"
    cmds = ['ip -d link show %s | grep -o "prog/xdp.*\\|xdpgeneric.*\\|xdpdrv.*" || echo no-xdp' % IF,
            'curl -s -m 3 -H "X-aws-ec2-metadata-token: %s" http://[fd00:ec2::254]/latest/meta-data/instance-id; echo' % tok,
            'getent ahosts s3.dualstack.us-east-1.amazonaws.com | head -2',
            'getent ahosts ssm.us-east-1.api.aws | head -2',
            "curl -s -m 5 -o /dev/null -w 'ssm endpoint http %{http_code} in %{time_total}s\\n' https://ssm.us-east-1.api.aws/",
            'date > /tmp/chk.txt && AWS_USE_DUALSTACK_ENDPOINT=true aws s3 cp --only-show-errors /tmp/chk.txt s3://%s/xdp/check/$(date +%%s).txt && echo s3 put ok' % b,
            'systemctl is-active amazon-ssm-agent',
            'tail -n 400 /var/log/amazon/ssm/amazon-ssm-agent.log | grep -ciE "error|fail" ; tail -n 5 /var/log/amazon/ssm/amazon-ssm-agent.log',
            "ss -tnp | grep -E 'ssm|:443' | head", 'ss -ltn | grep -E ":22 |:9000 |:7100 |:7000 "']
    return ''.join(sh(c, 30) for c in cmds)


def poweroff(q, peer):
    subprocess.Popen(['bash', '-c', 'sleep 5; %s/xsrv detach --if %s; shutdown -h now' % (BIN, IF)])
    return 'bye\n'


def ping(q, peer):
    return 'pong %s\n' % ('BUILD_OK' if os.path.exists(D + '/BUILD_OK') else 'BUILD_FAILED' if os.path.exists(D + '/BUILD_FAILED') else 'BUILDING')


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        peer = self.client_address[0]
        try:
            f = {'/ping': ping, '/facts': facts, '/nic': nic, '/start': start, '/stop': stop, '/check': check, '/poweroff': poweroff}[u.path]
            body = f(q, peer).encode()
            code = 200
        except Exception as e:
            body = ('error %r\n' % e).encode()
            code = 500
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class Srv(socketserver.TCPServer):  # one request at a time: the master is sequential
    address_family = socket.AF_INET6
    allow_reuse_address = True


os.makedirs(D + '/runs', exist_ok=True)
say('srvctl up IF=%s ORIG_MTU=%d MAXQ=%d' % (IF, ORIG_MTU, MAXQ))
Srv(('::', 9000), H).serve_forever()
