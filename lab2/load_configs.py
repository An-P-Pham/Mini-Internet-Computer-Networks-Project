import sys
import os
import subprocess

inputArg = sys.argv[1]
configsPath = os.getcwd() + '/' + inputArg + '/'
totalRouters = []

#get all the routers within I2
for routerFolder in os.listdir(configsPath):
    fullPath = configsPath + routerFolder
    if os.path.isdir(fullPath):
        if routerFolder == 'EXAMPLE':
            continue
        totalRouters.append(routerFolder)
totalRouters.sort()

#configure the interfaces
for router in totalRouters:
    zebraFilePath = configsPath + router +'/zebra.conf.sav'
    zebraFile = open(zebraFilePath, "r")
    zebraFileLines = zebraFile.readlines()
    zebraFile.close()
    iFace = ''
    ipAddr = ''
    for i in range(len(zebraFileLines)):
        zLine = zebraFileLines[i]
        if 'interface' in zLine:
            if 'interface lo\n' != zLine:
                iFace = zLine.rstrip('\n')
        if ' ip address' in zLine:
            ipAddr = zLine.rstrip('\n').lstrip(' ')
            cmd = 'vtysh -c "conf t" -c "' + iFace + '" -c "' + ipAddr + '"'
            currProc = subprocess.Popen(['./go_to.sh', router], stdin=subprocess.PIPE)
            currProc.communicate(cmd)


#configure ospf
for router in totalRouters:
    ospfdFilePath = configsPath + router + '/ospfd.conf.sav'
    ospfdFile = open(ospfdFilePath, "r")
    ospfdFileLines = ospfdFile.readlines()
    ospfdFile.close()
    ospfRouter = ''
    ospfNet = ''
    iFace = ''
    linkCost = ''
    for i in range(len(ospfdFileLines)):
        oLine = ospfdFileLines[i]
        if 'interface' in oLine:
            if 'interface lo\n' != oLine:
                iFace = oLine.rstrip('\n')
        if ' ip ospf' in oLine:
            linkCost = oLine.rstrip('\n').lstrip(' ')
            cmdLink = 'vtysh -c "conf t" -c "' + iFace + '" -c "' + linkCost + '"'
            currLinkProc = subprocess.Popen(['./go_to.sh', router], stdin=subprocess.PIPE)
            currLinkProc.communicate(cmdLink)
        if 'router' in oLine:
            ospfRouter = oLine.rstrip('\n')
        if 'network' in oLine:
            ospfNet = oLine.rstrip('\n').lstrip(' ')
            cmdNet = 'vtysh -c "conf t" -c "' + ospfRouter + '" -c "' + ospfNet + '"'
            currNetProc = subprocess.Popen(['./go_to.sh', router], stdin=subprocess.PIPE)
            currNetProc.communicate(cmdNet)
