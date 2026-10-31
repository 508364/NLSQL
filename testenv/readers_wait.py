"""带 wire 就绪重试的多进程读者（确保连接成功再压）"""
import pymysql, time, random, multiprocessing as mp

PORT = 13319

def conn():
    return pymysql.connect(host='127.0.0.1', port=PORT, user='root', password='',
                           database='sdb', connect_timeout=5, read_timeout=8,
                           write_timeout=8, charset='utf8mb4')

def worker(k, stop_evt, out_q):
    random.seed(k)
    ops = 0; errs = 0; last_err = None
    c = None
    for _ in range(100):   # 等 wire 就绪
        try:
            c = conn(); break
        except Exception:
            time.sleep(0.1)
    if c is None:
        out_q.put(('init_err', k, 'cannot connect')); return
    cur = c.cursor()
    while not stop_evt.is_set():
        try:
            cur.execute("SELECT v, s FROM t WHERE id=%d" % random.randrange(1500000))
            cur.fetchone()
            ops += 1
        except Exception as e:
            errs += 1
            if last_err is None:
                last_err = repr(e)
            if errs > 200:
                break
    try: c.close()
    except: pass
    out_q.put(('done', k, ops, errs, last_err))

if __name__ == '__main__':
    stop_evt = mp.Event()
    out_q = mp.Queue()
    ps = [mp.Process(target=worker, args=(k, stop_evt, out_q)) for k in range(3)]
    t_start = time.time()
    [p.start() for p in ps]
    time.sleep(10)
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
