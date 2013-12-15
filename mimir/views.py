from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.contrib.auth.decorators import login_required

from lessons.models import Lesson, LessonFollowsFromLesson, Question
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion
from django.contrib.auth.models import User
from django.core.urlresolvers import reverse
from django.db.models import Count
import json
import string
import datetime, time, calendar
from community.models import UserQuestion
from search import get_query

def index(request):
    if request.user.is_authenticated():
        curuser = request.user
        user_lessons = Lesson.objects.filter(usertakeslesson__user = curuser).distinct()[:5] #Gets five lessons that have been taken by the user
        whats_next = Lesson.objects.filter(preparation__leads_from__usertakeslesson__user = request.user).exclude(usertakeslesson__user = request.user).distinct().order_by('?')[:5]
        test_me = Lesson.objects.filter(usertakeslesson__user = curuser).order_by('?')[:1]
    else:
        user_lessons = []
        whats_next = []
        test_me = []
    context = ({
        'user_lessons':user_lessons, 'whats_next':whats_next, 'test_me':test_me,
    })
    return render(request, 'home.html', context)

def splash(request):
    if request.user.is_authenticated():
        return HttpResponseRedirect("/home/")
    return render(request, 'splash.html')

@login_required
def whatsnext(request):
    curuser = request.user
    whats_next = Lesson.objects.filter(preparation__leads_from__usertakeslesson__user = request.user).exclude(usertakeslesson__user = request.user).distinct().order_by('?')[:5]
    lessons_left = Lesson.objects.all().exclude(usertakeslesson__user = curuser).exclude(preparation__leads_from__usertakeslesson__user = request.user)
    context = ({
        'whats_next':whats_next, 'lessons_left':lessons_left
    })
    return render(request, 'next.html', context)

@login_required
def myskills(request):
    curuser = request.user
    user_lessons = UserTakesLesson.objects.filter(user = curuser).order_by('-date')
    context = ({
        'user_lessons':user_lessons, 
    })
    return render(request, 'skills.html', context)

def search(request, item):
    item = item.replace("_", " ")
    lessons = Lesson.objects.filter(get_query(item, ["name",]))
    questions = UserQuestion.objects.filter(get_query(item, ["title",]))
    users = User.objects.filter(get_query(item, ["username",]))
    context = ({
        'search_term': item,
        'lessons': lessons,
        'questions': questions,
        'users': users,
    })
    return render(request, 'search.html', context)
