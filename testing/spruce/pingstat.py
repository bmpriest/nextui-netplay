"""What a ping log says about Netplay's input delay: loss and round-trip
percentiles, then for each delay the share of frames whose input would arrive
late (half the round trip longer than the delay's frames at 60 fps).
  pingstat.py PING_OUTPUT.txt...     (ping -c 600 -i 0.1 PEER > PING_OUTPUT.txt)"""
import re,sys
for f in sys.argv[1:]:
    t=[float(x) for x in re.findall(r'time=([\d.]+)',open(f).read())]
    n=int(re.search(r'(\d+) packets transmitted',open(f).read()).group(1))
    s=sorted(t); q=lambda p:s[min(len(s)-1,int(p*len(s)))]
    lost=n-len(t)
    print(f"{f}: n={n} lost={lost} ({100*lost/n:.1f}%) p50={q(.5):.0f} p90={q(.9):.0f} p95={q(.95):.0f} p99={q(.99):.0f} max={s[-1]:.0f} ms")
    for d in (3,4,5,6,8,10,12):
        late=sum(1 for x in t if x/2>d*16.67)
        print(f"  delay {d:2}: {100*late/n:5.1f}% late, {100*(late+lost)/n:5.1f}% incl. lost")
