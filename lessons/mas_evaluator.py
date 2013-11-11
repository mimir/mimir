import math
import operator
#from sage.all import *
from pyparsing import Literal,CaselessLiteral,Word,Group,Optional,\
    ZeroOrMore,Forward,nums,alphas,Regex,ParseException
from inspect import getargspec
from re import match
from mas_latex import astToLatex
from sympy import *

#TODO Convert functions to be checks for operators in the db
#TODO Calculate the tree breadth first (or in a less retarded way)
#TODO Make division use Rational() when appropriate
#TODO Check diff question heavily, seems to be not evaluating in a sensible way
def solution_from_ast(ast):
    solution = Solution()
    solution.addStep(ast)
    solution.answer = evaluateNode(ast, ast, solution)
    return solution

def value_from_ast(ast):
    if ast.token == "FUNC":
        fn = eval(ast.value) #Get sympy function
        eval_children = [] 
        for node in ast.children:
            eval_children.append(value_from_ast(node)) #(node.value)
        debug = "" #Debug argument print
        for arg in eval_children:
            debug += str(arg) + ", "
        print "Calling: " + str(ast.value) + " with (" + debug + "\b\b)"
        return fn( *eval_children ) #ast.value
    
    elif ast.value == 'unary -': #If the node is a minus with a single argument
        return -value_from_ast(ast.children[0]) #ast.value

    elif ast.token == "OP": #If the node is a simple operator
        op1 = value_from_ast(ast.children[0])
        op2 = value_from_ast(ast.children[1])
        if opn[ast.value] == operator.truediv and op2 == 0:
            return float('nan')
        return opn[ast.value]( op1, op2 )#ast.value
    
    else:
        return ast.value

def evaluateNode( op, ast, solution ):
    '''
    if op.token == "FUNC": #If the node is a function
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
    '''
    
    if op.token == "FUNC":
        fn = eval(op.value) #Get sympy function
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
        op.value = fn( *eval_children )
        op.children = []
        solution.addStep(ast)
        return op.value
    
    elif op.value == 'unary -': #If the node is a minus with a single argument
        op.value = -evaluateNode(op.children[0], ast, solution)
        op.children = []
        return op.value

    elif op.token == "OP": #If the node is a simple operator
        op1 = evaluateNode( op.children[0], ast, solution )
        op2 = evaluateNode( op.children[1], ast, solution )
        print op
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
    wrongAnswers = None
    
    def __init__(self):
        self.answer = None
        self.steps = []
        self.subSteps = []

    def addStep(self, step):
        self.steps.append(astToLatex(step))

    def addSubStep(self, step):
        self.subSteps.append(step)

    def setWrongAnswers(self, wrongAnsDict):
        self.wrongAnswers = wrongAnsDict
