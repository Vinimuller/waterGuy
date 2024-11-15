import requests
import time

TOKEN = "6436678943:AAHYfEqwJ0MRV6J74TZEjAAqdKNQeO9kurU"
chat_id = "6264510964"
message = "hello from your telegram bot"
url = f"https://api.telegram.org/bot{TOKEN}/sendMessage?chat_id={chat_id}&text={message}"
# print(requests.get(url).json()) # this sends the message
msgIds = []
status = 'idle'
subStatus = 'idle'

class eventClass:
    def __init__(self, day, hour, minute, action):
        self.day = day
        self.hour = hour
        self.minute = minute
        self.action = action
    def __str__(self):
        return (self.day + " " + self.hour + "h" + self.minute + " " + self.action + " ")

eventArr = []

def sendMsg(msg):
    url = f"https://api.telegram.org/bot{TOKEN}/sendMessage?chat_id={chat_id}&text={msg}"
    requests.get(url); # this sends the message
    return True

def getNewMsg():
    url = f"https://api.telegram.org/bot{TOKEN}/getUpdates"
    response = requests.get(url).json() # this sends the message
    newMsg = []
    for msg in response["result"]:
        if(msg["update_id"] not in msgIds):
            msgIds.append(msg["update_id"])
            if(msg['message']['date'] > (int(time.time())-60)):
                newMsg.append(msg)
    return newMsg

def addEvent(event):
    eventArr.append(event)
    return True

def rmEvent(day):
    result = True
    # search for index
    # delete index
    return result

def readEvents():
    eventStr = ''
    for event in eventArr:
        eventStr += str(event) + "\n"
    
    return eventStr

def stateAddEvent(msg):
    newStatus = status
    cmd = msg['message']['text'].split(" ")
    print(cmd)
    hourMinute = cmd[1].split("h")
    
    event = eventClass(cmd[0],hourMinute[0],hourMinute[1], cmd[2])

    addEvent(event)
    print(event)

    sendMsg("Evento adicionado: " + str(event))
    return newStatus

def stateRmEvent(msg):
    eventStr = 'Eventos deletados: '
    newStatus = status
    day = msg['message']['text'].split(" ")[0]
    
    for event in eventArr:
        if(event.day == day):
            eventArr.remove(event)
            eventStr += str(event) + "\n"

    sendMsg(eventStr)
    return newStatus


def stateConfig(msg):
    newStatus = status
    global subStatus
    cmd = msg['message']['text']

    if(subStatus == 'addEvent'):
        stateAddEvent(msg)
        subStatus = 'idle'
        newStatus = 'idle'
    elif(subStatus == 'rmEvent'):
        stateRmEvent(msg)
        subStatus = 'idle'
        newStatus = 'idle'
    elif(subStatus == 'idle'):
        if(cmd == '/add_event'):
            print('add event')
            subStatus = 'addEvent'
            sendMsg("Digite o dia da semana, horario, minuto e ação: Segunda 13h00 Ligar")
        elif(cmd == '/rm_event'):
            print('rm event')
            subStatus = 'rmEvent'
            sendMsg("Digite o dia da semana a ser removido: Segunda")

    return newStatus

def stateIdle(msg):
    newStatus = status
    cmd = msg['message']['text']
    if(cmd == '/config'):
        print('into config state')
        # print current schedule
        # print options /add_event /rm_event
        sendMsg("Você entrou no modo de configuração. Eventos atualmente configurados: \n" + readEvents() + "Comandos disponíveis: \n/add_event \n/rm_event \n/exit")
        newStatus = 'config'

    return newStatus    


while True:

    newMsgArr = getNewMsg()

    for msg in newMsgArr:
        # print(msg)
        if(msg['message']['text'] == '/exit'):
            status = 'idle'
            sendMsg("Back to start")
        elif(status == 'config'):
            # config routine
            print('config state')
            status = stateConfig(msg)
        elif((status == 'idle')):
            print('idle state')
            status = stateIdle(msg)
            
    time.sleep(1)

