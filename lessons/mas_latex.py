import operator
#from sage.all import *
from sympy.printing.latex import latex

#TODO Make this output in a far nicer way
def astToLatex(ast): #Converts an AST to a lovely LaTeX form
    #TODO Make ** output as ^, or make MathJAX output ** as ^

    if not ast.children: #If it is a value, return it
        try:
            pretty = latex(ast.value, mode="plain")
            pretty = str(pretty).replace("\\",r"\\")
            return pretty
        except:
            return str(ast.value)

    #Deal with all of the standard operator formats
    elif ast.value == "/":
        return r"\\frac{" + str(astToLatex(ast.children[0])) + "}{" + str(astToLatex(ast.children[1])) + "}"
    elif ast.value == "*":
        return r"\\left( " + str(astToLatex(ast.children[0])) + r"\\times" + str(astToLatex(ast.children[1])) + r" \\right)"
    elif ast.value == "^":
        return str(astToLatex(ast.children[0])) + r"^{" + str(astToLatex(ast.children[1])) + r"}"
    elif ast.value == "unary -":
        return r"- \\left(" + str(astToLatex(ast.children[0])) + r" \\right)"
    elif ast.token == "OP":
        return r"\\left( " + str(astToLatex(ast.children[0])) + str(ast.value) + str(astToLatex(ast.children[1])) + r" \\right)"
    
    


    else: #If all else fails, use the operatorname notation (amsmath)
        args = ""
        for child in ast.children:
            args += str(astToLatex(child)) + ", "
        args = args[:-2]
        return r"\\operatorname{%s}\\left(%s\\right)" % (str(ast.value), args)
    '''
    elif ast.value == definite_integral: #If the function is a definite integral
        return r"\\int^" + str(astToLatex(ast.children[2])) + "_" + str(astToLatex(ast.children[3])) + " " + str(astToLatex(ast.children[0])) + " d" + str(astToLatex(ast.children[1]))
    elif ast.value == indefinite_integral:
        return r"\\int" + str(astToLatex(ast.children[0])) + " d" + str(astToLatex(ast.children[1]))
    '''
    

#TODO Make this better in pretty much every way
def valueToBestForm(val): #Converts a leaf node to a non-stupid form, i.e. no 48.0 jazz
    if isinstance(val, basestring): #If the value is a string, convert to a nice form
        try:
            return int(val)
        except ValueError:
            pass
        try:
            return float(val)
        except TypeError:
            pass
        return str(val)
    else:
        try:
            if abs(int(val) - float(val)) <= 0.000000000000001: #If it isn't a string, assume value and try to pick the best one
                return int(val)
            else:
                return float(val)
        except ValueError:
            pass

        return str(val) #If all else fails return a string
