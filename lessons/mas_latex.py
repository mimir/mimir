import operator

def astToLatex(ast): #Converts an AST to a lovely LaTeX form
    if not ast.children: #If it is a value, return it
        return valueToBestForm(ast.value)

    #Deal with all of the standard operator formats
    elif ast.value == "+":
        return str(astToLatex(ast.children[1])) + "+" + str(astToLatex(ast.children[0]))
    elif ast.value == "-":
        return str(astToLatex(ast.children[1])) + "-" + str(astToLatex(ast.children[0]))
    elif ast.value == "*":
        return str(astToLatex(ast.children[1])) + "*" + str(astToLatex(ast.children[0]))
    elif ast.value == "^":
        return str(astToLatex(ast.children[1])) + "^" + str(astToLatex(ast.children[0]))
    elif ast.value == "/":
        return r"\\frac{" + str(astToLatex(ast.children[1])) + "}{" + str(astToLatex(ast.children[0])) + "}"
    elif ast.value == "unary -":
        return "- " + str(astToLatex(ast.children[0]))
        

    else: #Else, if it is an unknown thing, use a function style
        args = ""
        for child in ast.children:
            args += str(astToLatex(child)) + ", "
        args = args[:-2]
        return r"\operatorname{%s}\left(%s\right)" % (str(ast.value), args)
    


def valueToBestForm(val): #Converts a leaf node to a non-stupid form, i.e. no 48.0 jazz
    try:
        return int(val)
    except ValueError:
        pass
    try:
        return float(val)
    except TypeError:
        pass
    return str(val)
