from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect
from lessons.models import Lesson
from user_profiles.models import UserProfile

def index(request):
    if request.user.is_authenticated():
        curuser = request.user
        user_lessons = Lesson.objects.filter(usertakeslesson__user = curuser)[:5] #Gets five lessons that have been taken by the user
        #TODO make the above line get the most recent five.
    else:
        user_lessons = []
    context = ({
        'user_lessons':user_lessons,
    })
    return render(request, 'home.html', context)

def splash(request):
    if request.user.is_authenticated():
        return HttpResponseRedirect("/home/")
    return render(request, 'splash.html')
