#! /usr/bin/env python

from zplot import *

ctype = 'eps' if len(sys.argv) < 2 else sys.argv[1]
#c = pdf('figure1.pdf')
c = canvas('pdf', title='leveldb-fillrandom-500k-1thread', dimensions=[100, 70])
t = table(file='fillrandom_500k.data')

d = drawable(canvas=c, coord=[21,16], xrange=[-0.5,t.getmax('rownumber')+0.5],
                yrange=[0,80], dimensions=[75, 50])

# because tics and axes are different, call axis() twice, once to
# specify x-axis, the other to specify y-axis
axis(d, linewidth=0.3, xtitle='db_bench value size (500k keys)', xtitlesize=5.5,
xmanual=t.query(select='value,rownumber'), xlabelfontsize=5.5,
ytitle='Throughput(MB/s)', ytitlesize=5.5, ylabelfontsize=5.5, yauto=[0,80,20],
ticmajorsize=2, xlabelshift=[0,2], xtitleshift=[0,1])

#grid(drawable=d, x=False, yrange=[20,60], ystep=20, linecolor='lightgrey', linedash=[2,2], linewidth=0.3)

p = plotter()
L = legend()

barargs = {'drawable':d, 'table':t, 'xfield':'rownumber',
           'linewidth':0.2, 'fill':True, 'barwidth':0.7,
		   'legend':L, 'stackfields':[]}

barargs['yfield'] = 'ext4dax'
barargs['legendtext'] = 'ext4-DAX'
barargs['fillcolor'] = 'gray'
barargs['fillstyle'] = 'solid'
barargs['fillsize'] = '0.5'
barargs['fillskip'] = '0.5'
barargs['cluster'] = [0,5]
p.verticalbars(**barargs)

barargs['yfield'] = 'splitfs'
barargs['legendtext'] = 'SplitFS'
barargs['fillcolor'] = 'bisque'
barargs['fillstyle'] = 'solid'
barargs['fillsize'] = '0.5'
barargs['fillskip'] = '0.5'
barargs['cluster'] = [1,5]
p.verticalbars(**barargs)

barargs['yfield'] = 'devfs'
barargs['legendtext'] = 'DevFS'
barargs['fillcolor'] = 'orange'
barargs['fillstyle'] = 'solid'
barargs['fillsize'] = '0.5'
barargs['fillskip'] = '0.5'
barargs['cluster'] = [2,5]
p.verticalbars(**barargs)

barargs['yfield'] = 'macrofs'
barargs['legendtext'] = 'CompoundFS'
barargs['fillcolor'] = 'peru'
barargs['fillstyle'] = 'dline1'
barargs['fillsize'] = '0.6'
barargs['fillskip'] = '1.8'
barargs['cluster'] = [3,5]
p.verticalbars(**barargs)

barargs['yfield'] = 'macrofs_slowdown'
barargs['legendtext'] = 'CompoundFS-slowcpu'
barargs['fillcolor'] = 'peru'
barargs['fillstyle'] = 'dline2'
barargs['fillsize'] = '0.6'
barargs['fillskip'] = '1.8'
barargs['cluster'] = [4,5]
p.verticalbars(**barargs)

#L.draw(c, coord=[d.left()+50, d.top()-2], width=4, height=4, fontsize=4, skipnext=3, hspace=1, skipspace=30)

c.render()
