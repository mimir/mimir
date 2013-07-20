from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.contrib.auth.forms import UserCreationForm
from django.contrib.auth import authenticate, login
from lessons.models import Lesson

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

def register(request):
    if request.method == 'POST':
        form = UserCreationForm(request.POST)
        if form.is_valid():
            new_user = form.save()
            username = request.POST['username']
            password = request.POST['password1']
            user = authenticate(username=username, password=password)
            login(request, user)
            return HttpResponseRedirect("/home/")
    else:
        form = UserCreationForm()
    return render(request, "registration/register.html", {
        'form': form,
    })

def splash(request):
    if request.user.is_authenticated():
        return HttpResponseRedirect("/home/")
    return render(request, 'splash.html')
