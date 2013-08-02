from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.core.urlresolvers import reverse
from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson
from lessons.generate import generateQuestion
from user_profiles.models import UserProfile
from community.models import UserQuestion, UserAnswer, UserComment, UserRating
import json
from django.core import serializers
from django.contrib.auth.models import User
from community.forum import JSONUserQuestion, JSONUserAnswer, JSONUserComment, listToJSON

def index(request):
    #Send list of all questions to page.
    #TODO: Send most recent, most popular, highest rated, etc via AJAX
    question_list = UserQuestion.objects.all()
    json_question_list = []
    for question in question_list:
        json_question = JSONUserQuestion(question)
        json_question_list.append(json_question.toJSON())
    context = ({
        'question_list':listToJSON(json_question_list),
    })
    return render(request, 'community/index.html', context)

def question(request, question_id):
    #Get question and convert to JSON
    question = get_object_or_404(UserQuestion, pk = question_id)
    json_question = JSONUserQuestion(question)
    
    #Get question comments and convert to JSON
    q_comments = list(UserComment.objects.filter(user_question = question_id))
    json_q_comments = []
    for comment in q_comments:
        json_comment = JSONUserComment(comment)
        json_q_comments.append(json_comment.toJSON())
    
    
    #Get answers and comments and convert to JSON
    answers = UserAnswer.objects.filter(user_question = question_id)
    json_answers = []
    json_a_comments = []
    answer_index = 0
    for answer in answers:
        json_answer = JSONUserAnswer(answer)
        json_answers.append(json_answer.toJSON())
        a_comments = UserComment.objects.filter(user_answer = answer.pk)
        json_comments = []
        for comment in a_comments:
            json_comment = JSONUserComment(comment)
            json_comments.append(json_comment.toJSON())
        json_a_comments.append(listToJSON(json_comments))
    
    #profile = UserProfile.objects.get(user = question.user.pk)
    
    
    context = ({'question': json_question.toJSON(), 'answers': listToJSON(json_answers), 'q_comments': listToJSON(json_q_comments), 'a_comments': listToJSON(json_a_comments), })
    return render(request, 'community/question.html', context)
#question(showing one question and the comments/answers associated with it)
#section(showing the most recent questions for either lesson, question, misc, etc)
#search(page to do searches and display results)?