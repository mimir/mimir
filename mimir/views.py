from django import forms
from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect

from lessons.models import Lesson, LessonFollowsFromLesson
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion
from django.contrib.auth.models import User
from django.db.models import Count
import json
import string
import datetime, time, calendar

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
    try: #Should really add checks to ensure none authenticated users cannot access the page or are redirected to a register/sign in page
        cur_user_p = UserProfile.objects.get(user__id = request.user.pk) #Get their profile
        lessons = UserTakesLesson.objects.filter(user = request.user.pk).order_by("date") #Get the lessons they have taken
        questions = UserAnswersQuestion.objects.filter(user = request.user.pk) #Get the questions they have answered
        
        #Calculate question stuff
        num_answered = len(questions)
        num_lessons = lessons.count()
        unique_lessons = lessons.values("lesson").distinct().count() #This may or may not work
        num_answered = questions.count()
        
        percentage = None #Percent correct
        if num_answered == 0: #Prevent division by 0
            percentage = " - "
        else:
            num_correct = questions.filter(correct = True).count()
            percentage = float(num_correct)/float(num_answered) * 100

        #Convert the queries to lists so that they can be manipulated for graphing
        lessons = list(lessons.values("date").order_by().annotate(Count('date'))) #The count is essentially useless, should really remove
        questions = list(questions.values("date").order_by().annotate(Count('date')))
        
        #Get plot data for lessons
        lesson_graph = []
        previous_date = 0
        previous_index = -1
        for x in lessons:
            date_millis = time.mktime(x['date'].date().timetuple())*1000
            if previous_date == date_millis:
                lesson_graph[previous_index][1] += 1
            else:				
                lesson_graph.append([date_millis, int(x['date__count'])])
                previous_index += 1
                previous_date = date_millis

        #Get plot data for questions
        question_graph = []
        previous_date = 0
        previous_index = -1
        for x in questions:
            date_millis = time.mktime(x['date'].date().timetuple())*1000
            if previous_date == date_millis:
                question_graph[previous_index][1] += 1
            else:				
                question_graph.append([date_millis, int(x['date__count'])])
                previous_index += 1
                previous_date = date_millis

        context = ({'cur_user': request.user, 'cur_user_p': cur_user_p, 'num_lessons': num_lessons, 'unique_lessons': unique_lessons, 'num_answered': num_answered, 'percent_correct': percentage, 'lessons': json.dumps(lesson_graph), 'questions': json.dumps(question_graph), })
    except User.DoesNotExist, UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'profile.html', context)
