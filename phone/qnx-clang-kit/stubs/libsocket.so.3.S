.syntax unified
.text
.globl bind
.type bind,%function
bind: bx lr
.globl connect
.type connect,%function
connect: bx lr
.globl getsockopt
.type getsockopt,%function
getsockopt: bx lr
.globl recv
.type recv,%function
recv: bx lr
.globl recvfrom
.type recvfrom,%function
recvfrom: bx lr
.globl send
.type send,%function
send: bx lr
.globl sendto
.type sendto,%function
sendto: bx lr
.globl socket
.type socket,%function
socket: bx lr
.globl setsockopt
.type setsockopt,%function
setsockopt: bx lr
.data
