#This file is a wrapper for the mas system.
#Calls to the mas system from outside should use these functions to do so.

from mas_parser import parse
from mas_evaluator import evaluateAST

def parseToAST(question):
    return parse(question)

def evaluateAST(root):
    return evaluateAST(root)
