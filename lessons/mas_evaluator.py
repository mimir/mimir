import math
import operator
from sage.all import *
from pyparsing import Literal,CaselessLiteral,Word,Group,Optional,\
    ZeroOrMore,Forward,nums,alphas,Regex,ParseException
from inspect import getargspec
from re import match

def evaluateNode( op ):
    if hasattr(op.value, "__call__"): #If the node is a function
        eval_children = []
        for node in op.children:
            eval_children.append(evaluateNode(node))
        debug = "" #Debug argument print
        for arg in eval_children:
            debug += str(arg) + ", "
        print "Calling: " + str(op.value) + " with (" + debug + "\b\b)"
        return op.value( *eval_children )
    
    #Remove these operators and actually use them like functions, assign the functions when building the tree
    elif op.value == 'unary -': #If the node is a minus with a single argument
        return -evaluateNode(op.children[0])

    elif str(op.value) in '+-*/^': #If the node is a simple operator
        op2 = evaluateNode( op.children[0] )
        op1 = evaluateNode( op.children[1] )
        return opn[op.value]( op1, op2 )
    
    else:
        return op.value

opn = { "+" : operator.add,
        "-" : operator.sub,
        "*" : operator.mul,
        "/" : operator.truediv,
        "^" : operator.pow }
