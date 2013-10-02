from django.shortcuts import render, get_object_or_404
from django.contrib.auth.decorators import login_required
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.core.urlresolvers import reverse

from lessons.mas_main import createQuestion, createSolution
from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson, Course
from user_profiles.models import UserTakesLesson, UserAnswersQuestion

from random import random
import datetime
import re
from decimal import *

'''
Notes:
next_link may seem completely pointless atm, and in truth it is, but when we get good (not completely random) question order sorted it will be used.
'''

def index(request):
    course_list = Course.objects.all()
    context = ({
        'course_list':course_list,
    })
    return render(request, 'lessons/index.html', context)

def read(request, lesson_url):
    lesson = get_object_or_404(Lesson, url__iexact = lesson_url)
    next_lessons = LessonFollowsFromLesson.objects.filter(leads_from__name = lesson.name).order_by('-strength')
    questions_exist = Question.objects.filter(lesson = lesson).exists()
    context = ({'lesson': lesson,'next_lessons':next_lessons,'questions_exist':questions_exist,})
    if request.user.is_authenticated():
        if not UserTakesLesson.objects.filter(user = request.user).filter(lesson = lesson).filter(date__gt = datetime.datetime.now() - datetime.timedelta(hours=1)).exists():
            user_takes = UserTakesLesson(user=request.user,lesson=lesson)
            user_takes.save()
    return render(request, 'lessons/read.html', context)

def rate_lesson(request, lesson_id): #TODO change this so lesson_id is passed via post maybe?
    if request.user.is_authenticated():
        p = request.POST
        if "rating" in p or "comment" in p:
            user_takes_lesson = list(UserTakesLesson.objects.filter(lesson__id = lesson_id, user = request.user).order_by('-date')[:1])
            if user_takes_lesson:
                if "rating" in p:
                    try:
                        user_takes_lesson[0].rating = int(p["rating"])
                        if int(p["rating"]) > 5 or int(p["rating"]) < 1:
                            return HttpResponse(status='400')
                    except ValueError:
                         return HttpResponse(status='400') #If rating is not an integer, return a Bad Request to the client
                if "comment" in p:
                    user_takes_lesson[0].comment = p["comment"] #TODO implement this in the template and as above make sure no nasties can be placed in comments
                user_takes_lesson[0].save()
    return HttpResponse('')

'''
This view handles the POST request that the client side Javascript (using JQuery) sends to the server to check the user's answer to a question.
'''
#TODO: Move pretty much all answer checks over to the MAS itself, including checking the format and whatnot
def check_answer(request):
    p = request.POST
    if "question_id" in p and "answer" in p and "rand_seed" in p:
        question = get_object_or_404(Question, pk = int(p["question_id"])) #TODO make sure this makes sense, this should never go wrong in practice but who knows, possibly expand to return something more meaningful to ajax?

        if not re.match(question.answer_format.regex, p["answer"]):
            return HttpResponse('{"correct":false, "message":"Answer was not in the correct format, please correct this and try again."}', mimetype="application/json") #TODO make this message contain specifics about the format

        #TODO if user is logged in add a useranswersquestion here
        getcontext().prec = 12 #TODO make prec a global setting?
        correctAns = createSolution(Decimal(p["rand_seed"]), question.question, question.answer) #Get the question solution
        userAns = None
        
        try:
            userAns = (type(correctAns.answer))(p["answer"])
        except:
            userAns = createSolution(0, question.question, p["answer"]).answer #TODO Write an actual parser for user answers
        
        if userAns == correctAns.answer:
            if request.user.is_authenticated():
                user_answers = UserAnswersQuestion(question = question, user = request.user, question_seed = p["rand_seed"], correct = True, answer = correctAns.answer)
                user_answers.save()
            return HttpResponse('{"correct":true}', mimetype="application/json")
        else:
            #TODO mistake analysis system, no biggie
            message = "Oops, looks like you made a mistake."
            
            if str(userAns) in correctAns.wrongAnswers:
                message = correctAns.wrongAnswers[str(userAns)]
            else:
                message = "You've made a mistake, but we aren't sure where exactly."
            if request.user.is_authenticated():
                user_answers = UserAnswersQuestion(question = question, user = request.user, question_seed = p["rand_seed"], correct = False, answer = correctAns.answer)
                user_answers.save()

            jsonSteps = "["
            for step in correctAns.steps:
                jsonSteps += '"' + str(step) + '", '
            jsonSteps = jsonSteps[:-2] + "]"
            print '{"correct":false, "message":"' + message + '", "steps":' + jsonSteps + '}'
            return HttpResponse('{"correct":false, "message":"' + message + '", "steps":' + jsonSteps + '}', mimetype="application/json")
    return HttpResponse('')


def question(request, lesson_url, question_id):
    question = get_object_or_404(Question, pk = question_id)
    getcontext().prec = 12
    rand_seed = Decimal(str(random()))
    template = createQuestion(rand_seed, question.question)
    question.answer = createSolution(rand_seed, question.question, question.answer).answer #TODO So have to remove this when Alex Best is less of a bitch about having the answer displayed under the question.
    return render(request, 'lessons/question.html', {'question': question, 'template': template, 'next_link': reverse('lessons:rand_question', args=[lesson_url]),'rand_seed':rand_seed,})

def rand_question(request, lesson_url):
    lesson = get_object_or_404(Lesson, url__iexact = lesson_url)
    question_l = list(Question.objects.filter(lesson = lesson).order_by('?')[:1])
    if question_l:
        return HttpResponseRedirect(reverse('lessons:question', args=[lesson_url, question_l[0].id]))
    return HttpResponseRedirect(reverse('lessons:read', args=[lesson_url]))

@login_required
def skill_tree(request):
    if request.user.is_authenticated():
        curuser = request.user
    lessons = Lesson.objects.all()
    links = LessonFollowsFromLesson.objects.all()
    context = ({
        'skill_tree':lessons, 'links':links,
    })
    return render(request, 'lessons/skilltree.html', context)
