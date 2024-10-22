import sys
import os
import subprocess

configsPath = os.getcwd() + '/configs/'
netinfoPath = os.getcwd() + '/netinfo/'

hostTorouter = {
}
loopbackAddr = {
}
totalRouters = []

#get all the routers within I2
for routerFolder in os.listdir(configsPath):
    fullPath = configsPath + routerFolder
    if os.path.isdir(fullPath):
        if routerFolder == 'EXAMPLE':
            continue
        totalRouters.append(routerFolder)
totalRouters.sort()        

#get interface information
linksFile = open(netinfoPath + 'links.csv', "r")
lines = linksFile.readlines()
linksFile.close()
for i in range(len(lines)):
    line = lines[i]
    if '-host' in line:
        info = line.split(',',7)
        hostTorouter[info[5]] = info[6]
        loopbackAddr[info[5]] = info[3]

#configure the hosts
configureIPCommand = 'sudo ifconfig '
addGateWayCommand = 'sudo route add default gw '
for router in totalRouters:
    currHostRouter = router + '-host'
    currHostConfig = configureIPCommand + router.lower() + ' ' + hostTorouter[router.lower()] + '/24 up'
    currGateWay = addGateWayCommand + loopbackAddr[router.lower()] + ' ' + router.lower()
    currProc = subprocess.Popen(['./go_to.sh', currHostRouter], stdin=subprocess.PIPE)
    currProc.communicate(currHostConfig)
    currProc = subprocess.Popen(['./go_to.sh', currHostRouter], stdin=subprocess.PIPE)
    currProc.communicate(currGateWay)