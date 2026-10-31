"""多进程只读压测（绕 GIL，每进程独立连接）+ 400ms RSS/CPU 采样"""
import pymysql, time, random, os, glob, multiprocessing as mp

PORT = 13319

def conn():
    return pymysql.connect(host='127.0.0.1', port=PORT, user='root', password='',
                           database='sdb', connect_timeout=5, read_timeout=8,
                           write_timeout=8, charset='utf8mb4')

def worker(k, stop_evt, out_q):
    random.seed(k)
    ops = 0; errs = 0; last_err = None
    try:
        c = conn(); cur = c.cursor()
    except Exception as e:
        out_q.put(('init_err', k, repr(e))); return
    while not stop_evt.is_set():
        try:
            rid = random.randrange(1500000)
            cur.execute("SELECT v, s FROM t WHERE id=%d" % rid)
            cur.fetchone()
            if ops % 2000 == 0:
                cur.execute("SELECT COUNT(*) FROM t")
                cur.fetchone()
            ops += 1
        except Exception as e:
            errs += 1
            if last_err is None:
                last_err = repr(e)
            if errs > 100:
                break
    try: c.close()
    except: pass
    out_q.put(('done', k, ops, errs, last_err))

if __name__ == '__main__':
    # 等 wire
    for _ in range(150):
        try:
            c = conn(); c.close(); break
        except Exception:
            time.sleep(0.1)

    stop_evt = mp.Event()
    out_q = mp.Queue()
    ps = [mp.Process(target=worker, args=(k, stop_evt, out_q)) for k in range(3)]
    t_start = time.time()
    [p.start() for p in ps]

    samples = []
    for i in range(25):
        time.sleep(0.4)
        try:
            pids = [int(p.split('/')[2]) for p in glob.glob('/proc/[0-9]*/comm')
                    if open(p).read().strip() == 'stress']
            if pids:
                pid = pids[0]
                rss = int(open('/proc/%d/status' % pid).read().split('VmRSS:')[1].split()[0])
                st = open('/proc/%d/stat' % pid).read().split()
                cpu = int(st[13]) + int(st[14])
                samples.append((round(time.time() - t_start, 1), rss, cpu))
        except Exception:
            pass

    stop_evt.set()
    [p.join() for p in ps]
    window = time.time() - t_start
    ops = errs = 0; details = []
    while not out_q.empty():
        m = out_q.get()
        details.append(m)
        if m[0] == 'done':
            ops += m[2]; errs += m[3]
    print("READ_OPS=%d QPS=%.0f ERRS=%d details=%s" % (ops, ops / window, errs, details))
    print("WINDOW=%.1fs samples(rssKB cpu_ticks)=%s" % (window, samples[:6] + samples[-3:]))
