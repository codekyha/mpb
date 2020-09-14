#datContent = [i.strip().split() for i in open("./var.dat").readlines()]
import os
import numpy
import matplotlib.pyplot as plt
from matplotlib import style
nobend = numpy.genfromtxt('makale.tm.dat', delimiter=', ', dtype=numpy.csingle)
numbands = len(nobend[0]) - 6
print(len(nobend[0])-6)
# freq = nobend[:,1]
k = nobend[1:,1]
print(k)
freqs = [0 + 0j] * (len(nobend)-1)
for x in xrange(1, len(nobend)):
#        for y in xrange(0, 11):
#         print '%d * %d = %d' % (x, y, x*y)
    freqs[x-1]=nobend[x][6:]
print(freqs[0])
print(freqs[5])

style.use('ggplot')
plt.plot(k,freqs,label='', linewidth=1)
#plt.plot(freqThz,R_dB,label='Reflectance',linewidth=1)
plt.title('w-k')
plt.ylabel('w(w*a/2*pi*c)')
plt.xlabel('k (k*a/2*pi)')
plt.legend()
plt.grid(True,color='w')
#plt.show()
savename= os.path.basename(os.getcwd())
print(savename)
#plt.savefig(savename+'.png')
plt.savefig(savename+'.png')
plt.show()
#with open('var.dat') as var:
#    yesbend_TR=[line.split(',')[2] for line in var]
#    yesbend_R=[line.split(',')[3] for line in var]
#print(yesbend_R)
wait = raw_input("Sorunsuz bitti.Lutfen bir tusa basin")
#pause()
#os.system("pause")
