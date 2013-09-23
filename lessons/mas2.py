from mas_evaluator import evaluateNode
from Queue import *
import math
import operator

'''Given a root node of an expression tree for an answer this code will traverse the tree and output a list of possible wrong answers with one mistake.'''
def wrong_answer_dict(root):
    wrong_ops = { "+": [operator.sub], 
                "-": [operator.add], } #TODO: Make more of these, eventually in a db??
    wrong_ans = dict()
    not_looked_at = LifoQueue()
    not_looked_at.put(root)
    while not not_looked_at.empty():
        node = not_looked_at.get()
        node_val = node.value
        if node_val in wrong_ops:
            for wrong_op in wrong_ops[str(node_val)]:
                node.value = wrong_op
                wrong_ans[str(evaluateNode(root))] = "Oops, did you do %s on %s, when you meant to do %s" % (op_to_str(wrong_op), str([evaluateNode(c) for c in node.children])[1:-1], node_val)
            node.value = node_val
        for child in node.children:
            not_looked_at.put(child)
    return wrong_ans

'''TODO: expand this code so it actually works properly
    for wrong_ops in ops[str(node.value)]:
        for child in root.children:
            incorrect_exprs.append()
'''

def op_to_str(op):
    ops = { operator.add : "+",
            operator.sub : "-",
            operator.mul : "*",
            operator.truediv : "/",
            operator.pow : "^" }
    if op in ops:
        return ops[op]
    return str(op)

