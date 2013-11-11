from mas_evaluator import value_from_ast
from Queue import *
import math
import operator

'''Given a root node of an expression tree for an answer this code will traverse the tree and outputs a dictionary of possible wrong answers obtained via one mistake. For each of these mistakes we record as the value for that key a message hinting at what possible mistake may have been made.'''
def wrong_answer_dict(root):
    wrong_ops = { "+": ["-"],
                "-": ["+"],
                "*": ["+", "/"],
                "/": ["*"], } #TODO: Make more of these, eventually in a db??
    wrong_ans = dict()
    
    #We BFS the tree getting things wrong as we traverse.
    not_looked_at = LifoQueue()
    not_looked_at.put(root)
    while not not_looked_at.empty():
        node = not_looked_at.get()
        node_val = node.value
        if node_val in wrong_ops: # If we have any common incorrect operations for the current operation.
            for wrong_op in wrong_ops[str(node_val)]:
                node.value = wrong_op
                current_wrong_answer = value_from_ast(root)
                print current_wrong_answer
                pretty_arguments = str.format("{0} and {1}",
                    ", ".join([str(value_from_ast(c)) for c in node.children[:-1]]),
                    str(value_from_ast(node.children[-1])),
                )
                wrong_ans[current_wrong_answer] = str.format("Oops, did you do {0} on {1}, when you meant to do {2}?",
                    op_to_str(wrong_op),
                    pretty_arguments,
                    node_val,
                )
            node.value = node_val
        for child in node.children:
            not_looked_at.put(child)
    return wrong_ans

def op_to_str(op):
    ops = { operator.add : "$+$",
            operator.sub : "$-$",
            operator.mul : "$\\times$",
            operator.truediv : "$\\div$",
            operator.pow : "^" }
    if op in ops:
        return ops[op]
    return str(op)

