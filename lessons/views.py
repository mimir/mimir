from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.core.urlresolvers import reverse
from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson
from user_profiles.models import UserTakesLesson

from lessons.generate import generateQuestion
from random import random

def index(request):
    latest_lesson_list = Lesson.objects.all()
    context = ({
        'latest_lesson_list':latest_lesson_list,
    })
    return render(request, 'lessons/index.html', context)

def read(request, lesson_name):
    lesson = get_object_or_404(Lesson, name__iexact = lesson_name.replace("_", " "))
    next_lessons = LessonFollowsFromLesson.objects.filter(leads_from__name = lesson.name).order_by('-strength')
    questions_exist = Question.objects.filter(lesson = lesson).exists()
    context = ({'lesson': lesson,'next_lessons':next_lessons,'questions_exist':questions_exist,})
    return render(request, 'lessons/read.html', context)

def question(request, lesson_name, question_id):
    question = get_object_or_404(Question, pk = question_id)
    pair = generateQuestion(random(), question.question, question.calculation)
    question.question = pair[0]
    question.answer = pair[1]
    return render(request, 'lessons/question.html', {'question': question,'next_link': reverse('lessons:rand_question', args=[lesson_name]),})

def rand_question(request, lesson_name):
    lesson = get_object_or_404(Lesson, name__iexact = lesson_name.replace("_", " "))
    question_l = list(Question.objects.filter(lesson = lesson).order_by('?')[:1])
    if question_l:
        question = question_l[0]
        pair = generateQuestion(random(), question.question, question.calculation)
        question.question = pair[0]
        question.answer = pair[1]
        return render(request, 'lessons/question.html', {'question': question,'next_link': reverse('lessons:rand_question', args=[lesson_name]),})
    return HttpResponseRedirect(reverse('lessons:read', args=[lesson_name]))

def skill_tree(request):
    if request.user.is_authenticated():
        curuser = request.user
    lessons = Lesson.objects.all()
    links = LessonFollowsFromLesson.objects.all()
    context = ({
        'skill_tree':lessons, 'links':links,
    })
    return render(request, 'lessons/skilltree.html', context)
