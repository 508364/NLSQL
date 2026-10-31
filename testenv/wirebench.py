"""wire 读性能与延迟基准（单/多连接，PyMySQL 口径）"""
import pymysql, threading, time, random, sys

PORT = 13317
STOP = False

def conn():
    return pymysql.connect(host='127.0.0.1', port=PORT, user='root', password='',
                           database='wdb', connect_timeout=5, read_timeout=10,
                           write_timeout=10, charset='utf8mb4')

# 等服务端
for _ in range(100):
    try:
        c = conn(); c.close(); break
    except Exception:
        time.sleep(0.1)

c = conn(); cur = c.cursor()
cur.execute('SELECT COUNT(*) FROM t'); print('COUNT', cur.fetchone())

# 1) 单连接串行延迟（网络往返口径）
N = 5000
t0 = time.time()
for i in range(N):
    cur.execute('SELECT v FROM t WHERE id=%d' % (i * 37 % 200000))
    cur.fetchone()
t1 = time.time()
dt = t1 - t0
print('SERIAL n=%d total=%.2fs qps=%.0f lat_us=%.1f' % (N, dt, N / dt, dt / N * 1e6))

# 2) 4 连接并发
ops = [0]*4; errs = [0]*4
def worker(k):
    random.seed(k)
    try:
        cc = conn(); cu = cc.cursor()
        while not STOP:
            cu.execute('SELECT v FROM t WHERE id=%d' % random.randrange(200000))
            cu.fetchone()
            ops[k] += 1
    except Exception as e:
        errs[k] += 1
ts = [threading.Thread(target=worker, args=(k,)) for k in range(4)]
t0 = time.time()
[t.start() for t in ts]
time.sleep(6)
STOP = True
[t.join() for t in ts]
dt = time.time() - t0
tot = sum(ops)
print('PAR4 total=%d qps=%.0f errs=%s' % (tot, tot / dt, errs))
try: c.close()
except: pass
