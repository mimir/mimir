from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect

from lessons.models import Lesson
from user_profiles.models import UserProfile
from user_profiles.models import UserTakesLesson
from user_profiles.models import UserAnswersQuestion
from django.contrib.auth.models import User

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

def profile(request): #Users own profile page
    try:
        cur_user_p = UserProfile.objects.get(user__id = request.user.pk) #Get their profile
        lessons = UserTakesLesson.objects.filter(user = request.user.pk) #Get the lessons they have taken
        questions = UserAnswersQuestion.objects.filter(user = request.user.pk) #Get the questions they have answered
        num_lessons = len(lessons)
        unique_lessons = len(lessons.values("lesson").distinct()) #This may or may not work
        num_answered = len(questions)
        percentage = None #Percent correct
        if num_answered == 0: #Prevent division by 0
            percentage = "- "
        else:
            num_correct = len(questions.filter(correct = True))
            percentage = float(num_correct)/float(num_answered) * 100
        context = ({'cur_user': request.user, 'cur_user_p': cur_user_p, 'num_lessons': num_lessons, 'unique_lessons': unique_lessons, 'num_answered': num_answered, 'percent_correct': percentage})
    except User.DoesNotExist, UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'profile.html', context)