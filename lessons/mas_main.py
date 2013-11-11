#This file is a wrapper for the mas system.
#Calls to the mas system from outside should use these functions to do so.

from mas_parser import parse
from mas_evaluator import solution_from_ast
from mas_generator import generateQuestion, generateAnswerTemplate
from mas_mistakes import wrong_answer_dict
import copy

def createQuestion(seed, template):
    template = generateQuestion(seed, template)
    return template

def createSolution(seed, template, answer):
    answerToParse = generateAnswerTemplate(seed, template, answer)
    print "--------Answer to parse---------"
    print answerToParse
    print "--------------------------------"
    ast = parse(answerToParse)
    wrong_answers = wrong_answer_dict(ast)
    solution = solution_from_ast(copy.deepcopy(ast))
    solution.setWrongAnswers(wrong_answers)
    return solution
