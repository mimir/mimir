from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.core.urlresolvers import reverse
from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson
from user_profiles.models import UserTakesLesson

from lessons.generate import generateQuestion
from random import random
import datetime

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
    if request.user.is_authenticated():
        if not UserTakesLesson.objects.filter(user = request.user).filter(lesson = lesson).filter(date__gt = datetime.datetime.now() - datetime.timedelta(hours=1)).exists():
            user_takes = UserTakesLesson(user=request.user,lesson=lesson)
            user_takes.save()
    return render(request, 'lessons/read.html', context)

def rate_lesson(request, lesson_id):
    if request.user.is_authenticated():
        p = request.POST
        if "rating" in p or "comment" in p:
            user_takes_lesson = list(UserTakesLesson.objects.filter(lesson__id = lesson_id, user = request.user).order_by('-date')[:1])
            if user_takes_lesson:
                if "rating" in p:
                    user_takes_lesson[0].rating = int(p["rating"]) #TODO make this properly handle non-integer ratings so that people can't break the site by submitting funny POST requests to it.
                if "comment" in p:
                    user_takes_lesson[0].comment = p["comment"] #TODO implement this in the template and as above make sure no nasties can be placed in comments
                user_takes_lesson[0].save()
    return HttpResponse('')

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
