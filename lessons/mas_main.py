#This file is a wrapper for the mas system.
#Calls to the mas system from outside should use these functions to do so.

from mas_parser import parse
from mas_evaluator import evaluateAST
from mas_generator import generateQuestion, generateAnswerTemplate
from mas_mistakes import wrong_answer_dict

def createQuestion(seed, template):
    template = generateQuestion(seed, template)
    return template

def createSolution(seed, template, answer):
    answerToParse = generateAnswerTemplate(seed, template, answer)
    ast = parse(answerToParse)
    solution = evaluateAST(copy.deepcopy(ast))
    solution.setWrongAnswers(wrong_answer_dict(ast))
    return solution


