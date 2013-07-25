from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect

from lessons.models import Lesson, LessonFollowsFromLesson
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion
from django.contrib.auth.models import User

def index(request):
    if request.user.is_authenticated():
        curuser = request.user
        user_lessons = Lesson.objects.filter(usertakeslesson__user = curuser).distinct()[:5] #Gets five lessons that have been taken by the user
        whats_next = Lesson.objects.filter(preparation__leads_from__usertakeslesson__user = request.user).exclude(usertakeslesson__user = request.user).distinct().order_by('?')[:5]
    else:
        user_lessons = []
        whats_next = []
    context = ({
        'user_lessons':user_lessons, 'whats_next':whats_next,
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
        num_lessons = lessons.count()
        unique_lessons = lessons.values("lesson").distinct().count() #This may or may not work
        num_answered = questions.count()
        percentage = None #Percent correct
        if num_answered == 0: #Prevent division by 0
            percentage = "- "
        else:
            num_correct = questions.filter(correct = True).count()
            percentage = float(num_correct)/float(num_answered) * 100
        context = ({'cur_user': request.user, 'cur_user_p': cur_user_p, 'num_lessons': num_lessons, 'unique_lessons': unique_lessons, 'num_answered': num_answered, 'percent_correct': percentage})
    except User.DoesNotExist, UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'profile.html', context)
