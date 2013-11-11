import math
import operator
#from sage.all import *
from pyparsing import Literal,CaselessLiteral,Word,Group,Optional,\
    ZeroOrMore,Forward,nums,alphas,Regex,ParseException
from inspect import getargspec
from re import match
from sympy import *

'''
Token Types
-----------
INT
FLOAT
VAR
CONST
OP
FUNC
'''


#TODO Change to use Sympy
#TODO Don't convert tokens to functions here, that is evaluation
#TODO Add more meta data to Node, i.e. what is the type (easier in the long run, less fucking about with regex)
#TODO Reverse all children so that everything is in the correct order (change orders of evaluator and LaTeX)
def parse(question):
    global exprStack
    exprStack = []
    global argCount
    argCount = 0
    try:
        createParser().parseString(question, parseAll=True)
    except ParseException as e:
        print "Parse Exception: " + str(e)
        return
    print "\n"
    return exprStack.pop()
    
class Node:
    token = None
    value = None
    children = None
    
    def __init__(self, token, value):
        self.token = token
        self.value = value
        self.children = []
        
    def addChild(self, node):
        self.children.append(node)
    
    def __str__(self):
        children_str = ""
        if self.children:
            children_str = "\n\t" +",\n\t".join([str(c) for c in self.children])
        return str.format("Token: {0}, value: {1}, children:[{2}]", str(self.token),  str(self.value), children_str)

exprStack = []
argCount = 0

def pushFirst( strg, loc, toks ):
    print toks[0]
    if toks[0] in opn: #If the token is a basic operator
        node = Node("OP", toks[0])
        node.addChild(exprStack.pop())
        node.addChild(exprStack.pop())
        node.children.reverse()
        exprStack.append(node)

    elif toks[0] == "INFINITY": #Recognise constants
        node = Node("CONST", infinity)
        exprStack.append(node)

    elif toks[0] == "PI": #TODO Add e back as a constant
        node = Node("CONST", pi)
        exprStack.append(node)

    elif match(r"[a-mo-zA-Z]$", str(toks[0])): #If token is a variable
        print "Found variable " + str(toks[0])
        node = Node("VAR", Symbol(str(toks[0])))
        exprStack.append(node)
    
    #Need a check to ensure a function is a function, that the function has

    elif match(r"[a-zA-Z]([a-zA-Z_$]+)?$", str(toks[0])): #New function test, leave as string
        node = Node("FUNC", str(toks[0]))
        global argCount
        while argCount != 0:
            print "Iteration: " + str(argCount)
            op = exprStack.pop()
            node.addChild(op)
            argCount -= 1
        node.children.reverse()
        exprStack.append(node)

    elif match(r"[+-]?\d+$", str(toks[0])): #If token is an integer
        node = Node("INT", int(toks[0]))
        exprStack.append(node)

    elif match(r"[+-]?\d+(\.\d+)?$", str(toks[0])): #If token is a float
        node = Node("FLOAT", float(toks[0]))
        exprStack.append(node)

    else:
        exprStack.append( Node(toks[0]) )
    
        
def pushUMinus( strg, loc, toks ):
    for t in toks:
      if t == '-':
        node = Node("OP", "unary -")
        node.addChild(exprStack.pop())
        exprStack.append(node)
      else:
        break

#TODO: Find out why argCount works and ensure it is robust
def incArgCount( strg, loc, toks ):
    global argCount
    argCount += 1
    print toks[0]
    print "ArgCount = " + str(argCount)

bnf = None
def createParser():
    """
    expop   :: '^'
    multop  :: '*' | '/'
    addop   :: '+' | '-'
    integer :: ['+' | '-'] '0'..'9'+
    atom    :: PI | E | real | fn '(' expr ')' | '(' expr ')'
    factor  :: atom [ expop factor ]*
    term    :: factor [ multop factor ]*
    expr    :: term [ addop term ]*
    """
    global bnf
    if not bnf:
        point = Literal( "." )
        e     = CaselessLiteral( "E" )
        comma = Literal( "," ).suppress()
        plus  = Literal( "+" )
        minus = Literal( "-" )
        mult  = Literal( "*" )
        div   = Literal( "/" )
        lpar  = Literal( "(" ).suppress()
        rpar  = Literal( ")" ).suppress()
        addop  = plus | minus
        multop = mult | div
        expop = Literal( "^" )
        pi    = CaselessLiteral( "PI" )
        infinity = CaselessLiteral( "INFINITY" )
        
        
        #~ fnumber = Combine( Word( "+-"+nums, nums ) + 
                           #~ Optional( point + Optional( Word( nums ) ) ) +
                           #~ Optional( e + Word( "+-"+nums, nums ) ) )
        fnumber = Regex(r"[+-]?\d+(\.\d+)?")
        ident = Word(alphas, alphas+nums+"_$")
        
        
        
        expr = Forward()
        func = ident + lpar + Group(expr).setParseAction( incArgCount ) + ZeroOrMore(comma + Group(expr).setParseAction( incArgCount )) + rpar 

        atom = ((0,None)*minus + ( pi | e | infinity | fnumber | func.setName("Function") | ident ).setParseAction( pushFirst ) | 
                (0,None)*minus + Group( lpar + expr + rpar )).setParseAction(pushUMinus) 
        
        # by defining exponentiation as "atom [ ^ factor ]..." instead of "atom [ ^ atom ]...", we get right-to-left exponents, instead of left-to-righ
        # that is, 2^3^2 = 2^(3^2), not (2^3)^2.
        factor = Forward()
        factor <<= atom + ZeroOrMore( ( expop + factor ).setParseAction( pushFirst ) )
        
        term = factor + ZeroOrMore( ( multop + factor ).setParseAction( pushFirst ) )
        expr <<= term + ZeroOrMore( ( addop + term ).setParseAction( pushFirst ) )
        bnf = expr
    return bnf

def getParser():
    return bnf

epsilon = 1e-12 #TODO: Add more operators (logical, equivalence, etc)
opn = { "+" : operator.add,
        "-" : operator.sub,
        "*" : operator.mul,
        "/" : operator.truediv,
        "^" : operator.pow }



