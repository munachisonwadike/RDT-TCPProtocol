import numpy as np
import matplotlib.pyplot as plt
 

y = np.genfromtxt('CWND')
x = [ i for i in range(np.shape(y)[0]) ]

 
plt.title('Example')
plt.ylabel('WINDOW SIZE')
plt.xlabel('ITERATION')

plt.plot(x, y) 
plt.show()
