import math
import operator
from sage.all import *
from pyparsing import Literal,CaselessLiteral,Word,Group,Optional,\
    ZeroOrMore,Forward,nums,alphas,Regex,ParseException
from inspect import getargspec
from re import match
from mas_latex import astToLatex

def evaluateAST( ast ):
    solution = Solution()
    solution.answer = evaluateNode( ast, ast, solution )
    return solution

def evaluateNode( op, ast, solution ):
    if hasattr(op.value, "__call__"): #If the node is a function
        eval_children = []
        for node in op.children:
            node.value = evaluateNode(node, ast, solution)
            node.children = []
            solution.addStep(ast)
            eval_children.append(node.value)
        debug = "" #Debug argument print
        for arg in eval_children:
            debug += str(arg) + ", "
        print "Calling: " + str(op.value) + " with (" + debug + "\b\b)"
        op.value = op.value( *eval_children )
        op.children = []
        solution.addStep(ast)
        return op.value
    
    elif op.value == 'unary -': #If the node is a minus with a single argument
        op.value = -evaluateNode(op.children[0], ast, solution)
        op.children = []
        return op.value

    elif str(op.value) in '+-*/^': #If the node is a simple operator
        op2 = evaluateNode( op.children[0], ast, solution )
        op1 = evaluateNode( op.children[1], ast, solution )
        op.value = opn[op.value]( op1, op2 ) #Change the nodes value
        op.children = []
        solution.addStep(ast) #Add a solution step
        return op.value
    
    else:
        return op.value

opn = { "+" : operator.add,
        "-" : operator.sub,
        "*" : operator.mul,
        "/" : operator.truediv,
        "^" : operator.pow }

class Solution:
    answer = None #The final correct answer to the question
    steps = None #Shows the full question at each step with one calculation done per step
    subSteps = None #Shows the specific part that has changed i.e Step - 6+12, Substep - 6*2=12
    
    def __init__(self):
        self.answer = None
        self.steps = []
        self.subSteps = []

    def addStep(self, step):
        self.steps.append(astToLatex(step))

    def addSubStep(self, step):
        self.subSteps.append(step)
