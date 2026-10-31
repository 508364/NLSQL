"""并发查询上限压测客户端（wire 协议，PyMySQL）。

对同一服务端按 1/2/4/8/16/32/64 个并发连接逐档压点查，报每档：
总 QPS、平均延迟、连接建立失败数、线程异常数。
用法：python3 conclim.py <port> [duration_s] [n1,n2,...]
"""
import pymysql, threading, time, sys, random

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 13351
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
NS = [int(x) for x in sys.argv[3].split(',')] if len(sys.argv) > 3 else [1, 2, 4, 8, 16, 32, 64]
ROWS = 50000


def conn():
    return pymysql.connect(host='127.0.0.1', port=PORT, user='root', password='',
                           database='cdb', connect_timeout=5, read_timeout=10,
                           write_timeout=10, charset='utf8mb4')


def main():
    total_ok = 0
    for n in NS:
        cs = []
        cerr = 0
        for _ in range(n):
            try:
                cs.append(conn())
            except Exception as e:
                cerr += 1
                if cerr == 1:
                    print('CONC conn_error %r' % (e,))
        if not cs:
            print('CONC phase n=%d FAIL connect' % n)
            continue

        bar = threading.Barrier(len(cs) + 1)
        ops = [0] * len(cs)
        us = [0.0] * len(cs)
        qerr = [0] * len(cs)
        stop = threading.Event()

        def worker(k, c):
            cur = c.cursor()
            rnd = random.Random(1000 + k)
            cnt = 0
            tot = 0.0
            lo = 1
            hi = ROWS
            bar.wait()
            while not stop.is_set():
                # 每 64 次查一次时钟，降低客户端开销
                for _ in range(64):
                    rid = rnd.randint(lo, hi)
                    t0 = time.perf_counter()
                    cur.execute("SELECT v FROM t WHERE id=%d" % rid)
                    row = cur.fetchone()
                    tot += (time.perf_counter() - t0) * 1e6
                    cnt += 1
                    if row is None:
                        qerr[k] += 1
                if cnt >= 65536:
                    break
            ops[k] = cnt
            us[k] = tot

        th = []
        t0 = time.perf_counter()
        for k, c in enumerate(cs):
            t = threading.Thread(target=worker, args=(k, c), daemon=True)
            t.start()
            th.append(t)
        bar.wait()
        while any(t.is_alive() for t in th) and time.perf_counter() - t0 < DUR:
            time.sleep(0.01)
        stop.set()
        for t in th:
            t.join()
        wall = time.perf_counter() - t0

        tot_ops = sum(ops)
        tot_us = sum(us)
        qerrs = sum(qerr)
        print('CONC phase n=%-3d conns=%d conn_err=%d ops=%d wall=%.2fs qps=%.0f avg_us=%.1f q_err=%d per_conn_qps=%.0f' %
              (n, len(cs), cerr, tot_ops, wall, tot_ops / wall if wall > 0 else 0,
               tot_us / tot_ops if tot_ops else 0, qerrs,
               (tot_ops / len(cs)) / wall if wall > 0 else 0))
        total_ok += 1
        for c in cs:
            try:
                c.close()
            except Exception:
                pass
        # 等旧连接的 cli 线程在服务端摘槽退场，避免下一档握手撞上瞬时上限
        time.sleep(0.6)
    print('CONC client_done phases_ok=%d' % total_ok)


if __name__ == '__main__':
    main()
