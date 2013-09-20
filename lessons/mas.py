import math
import operator
from sage.all import *
from pyparsing import Literal,CaselessLiteral,Word,Group,Optional,\
    ZeroOrMore,Forward,nums,alphas,Regex,ParseException
from inspect import getargspec
from re import match

exprStack = []
argCount = 0

#TODO: Rename and structure functions appropriately
#TODO: Seperate all paring from evaluating and create a wrapper API for using MAS
#TODO: Add error checking and useful exceptions

#TODO: Clean up pushFirst function
def pushFirst( strg, loc, toks ):
    print toks[0]
    if toks[0] in opn: #If the token is a basic operator
        node = Node(toks[0])
        node.addChild(exprStack.pop())
        node.addChild(exprStack.pop())
        exprStack.append(node)

    elif toks[0] == "INFINITY": #Recognise constants
        node = Node(infinity)
        exprStack.append(node)

    elif toks[0] == "PI":
        node = Node(pi)
        exprStack.append(node)

    elif toks[0] == "E":
        node = Node(sage_eval("e"))
        exprStack.append(node)

    elif match(r"[a-mo-zA-Z]$", str(toks[0])): #If token is a variable
        print "Found variable " + str(toks[0])
        node = Node(var(toks[0]))
        exprStack.append(node)
    
    #Need a check to ensure a function is a function, that the function has 

    elif match(r"[a-zA-Z]([a-zA-Z_$]+)?$", str(toks[0])): #If the token is an ident
        print "Matches as an ident."
        fn = None
        try:
            fn = sage_eval(toks[0])
        except NameError as e:
            pass
        if fn == None:        
            raise Exception("Identifier not recognised")
        fnnode = Node(fn)
        print "The function has been nodified."
        global argCount
        while argCount != 0:
            print "Iteration: " + str(argCount)
            op = exprStack.pop()
            fnnode.addChild(op)
            argCount -= 1
            #if len(exprStack) == 0:
            #    break
        exprStack.append(fnnode)
        print "Function tree pushed to stack."

    elif match(r"[+-]?\d+$", str(toks[0])): #If token is an integer
        node = Node(int(toks[0]))
        exprStack.append(node)

    elif match(r"[+-]?\d+(\.\d+)?$", str(toks[0])): #If token is a float
        node = Node(float(toks[0]))
        exprStack.append(node)

    else:
        exprStack.append( Node(toks[0]) )
    '''
    elif toks[0] in fn:
        node = Node(toks[0])
        node.addChild(exprStack.pop())
        exprStack.append(node)
    elif toks[0].name == "ident": #TODO: Make functions and constants work
        func = Node(toks[0])
        argnum = toks.count(",")
        while argnum != 0:
            func.addChild(exprStack.pop())
            if exprStack.peek() == ",":
                exprStack.pop()
            argnum -= 1
    '''
    
        
def pushUMinus( strg, loc, toks ):
    for t in toks:
      if t == '-':
        node = Node('unary -')
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
def BNF():
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
        factor << atom + ZeroOrMore( ( expop + factor ).setParseAction( pushFirst ) )
        
        term = factor + ZeroOrMore( ( multop + factor ).setParseAction( pushFirst ) )
        expr << term + ZeroOrMore( ( addop + term ).setParseAction( pushFirst ) )
        bnf = expr
    return bnf

# map operator symbols to corresponding arithmetic operations
epsilon = 1e-12 #TODO: Add more operators (logical, equivalence, etc)
opn = { "+" : operator.add,
        "-" : operator.sub,
        "*" : operator.mul,
        "/" : operator.truediv,
        "^" : operator.pow }

def evaluateNode( op ):
    if hasattr(op.value, "__call__"): #If the node is a function
        eval_children = []
        for node in op.children:
            eval_children.append(evaluateNode(node))
        eval_children.reverse() #TODO: Move this step to the point of building the tree
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

'''        
def evaluateNode( op ): #Make constants and functions work (probably check matchings with ident and var tokens)
    if hasattr(op.value, "__call__"):
        eval_children = []
        for node in op.children:
            eval_children.append(evaluateNode(node))
        return op.value(*eval_children)
    if op.value == 'unary -':
        return -evaluateNode( op.children[0] )
    if op.value in "+-*/^":
        op2 = evaluateNode( op.children[0] )
        op1 = evaluateNode( op.children[1] )
        return opn[op.value]( op1, op2 )
    if op.value.isalpha():
        try:
            return sage_eval(str(op.value).lower())
        except NameError as e:
            print e
            print "\n-------------------------------------"
        if len(str(op.value)) == 1:
            return var(op.value)
    ########################
    elif op.value == "PI":
        return math.pi # 3.1415926535
    elif op.value == "E":
        return math.e  # 2.718281828
    elif len(str(op.value)) == 1 and op.value.isalpha():
        return var(op.value)
    #########################
    if op.value in fn:
        return fn[op.value]( evaluateNode( op.children[0] ) )
    elif op.value.isalpha():
        raise Exception("invalid identifier '%s'" % op)
    else:
        return float( op.value )
'''

#TODO: Try to eliminate the amount of global variables
global steps
def parse(question):
    global exprStack
    exprStack = []
    global argCount
    argCount = 0
    global steps
    steps = [] #TODO: Make steps work
    try:
        BNF().parseString(question, parseAll=True)
    except ParseException as e:
        print "Parse Exception: " + str(e)
        return
    print "\n"
    root = exprStack.pop()
    #print printStep(root)
    return evaluateNode(root)

#TODO: Create a working treeToString function
'''
def treeToString(node):
    if len(node.children) == 2:
        return "( " + treeToString(node.children[0]) + " " + str(node.value) + " " + treeToString(node.children[1]) + " )"
    elif len(node.children) == 1:
        return "( " + str(node.value) + treeToString(node.children[0]) + " )"
    else:
        return str(node.value)
'''

def printStep(node):
    uneval = treeToString(node)
    return latex(uneval)

def treeToString(node):
    if False:
        pass #Exceptions to the hold=True rules
    elif len(node.children) != 0:
        treeed = []
        for node in node.children:
            treeed.append(treeToString(node)) #BUG: Could be backwards args
        return node.value(*treeed, hold=True)
    else:
        return node.value


class Node:
    value = None
    children = None
    
    def __init__(self, value):
        self.value = value
        self.children = []
        
    def addChild(self, node):
        self.children.append(node)
