import random
import string
import re #regex

variable_regex = r"\$\[\w+,\d+,\d+\]"

#Types-INT, DEC
def rand(seed, type, lower, upper): #Generates random values from a seed
    random.seed(seed)
    if type == "INT":
        return float(random.randint(lower, upper))
    elif type == "DEC":
        return float(lower + (random.random()*(upper-lower)))
    else:
        return 0

def generateQuestion(seed, template):
    #Recover variables
    #Assign variables values
    #Create actual question
    #Generate answer
    #Return
    variables = findVariables(template)
    variables = generateVariables(seed, variables)
    readableQuestion = templateToReadable(template, variables)
    return readableQuestion

def generateAnswerTemplate(seed, template, answer):
    variables = findVariables(template)
    variables = generateVariables(seed, variables)
    machineQuestion = generateAnswerCode(answer, variables)
    return machineQuestion

def findVariables(template): #TODO Remove variable declarations from the template to the new variables field
    variables = {}
    varList = re.findall(variable_regex + r"\w+", template)
    for x in xrange(len(varList)):
        temp = varList[x].split("[")
        vals = temp[1].split("]")
        variables[vals[1]] = vals[0]
    return variables

def generateVariables(seed, variables): #Generates the random values for the variables
    random.seed(seed)
    values = {}
    for x in variables:
        vals = variables[x].split(",")
        values[x] = rand(random.random(), vals[0], float(vals[1]), float(vals[2]))
    return values

def templateToReadable(template, variables): #Converts the template question into a readable question
    line = template
    for x in variables: #code to get amount of variables - len(re.findall(varRegex, template))
        line = re.sub(variable_regex + str(x), str(variables[x]), line) #Replaces variable positions with their actual values
    return line

def generateAnswerCode(answer, variables):
    generated = answer
    for var in variables:
        generated = re.sub(r"\$" + str(var), str(variables[var]), generated)
    return generated

