from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.core.urlresolvers import reverse
from django.contrib.auth import authenticate, login, logout
from django.contrib.auth.models import User
from user_profiles.forms import UserCreationForm, UserProfileForm
from user_profiles.models import UserProfile, UserTakesLesson, UserAnswersQuestion
from django.db.models import Count
import json
import string
import datetime, time, calendar

def index(request):
    user_list = User.objects.all()
    context = ({
        'user_list':user_list,
    })
    return render(request, 'user_profiles/index.html', context)

def view(request, user_name):
    cur_user = get_object_or_404(User, username = user_name)
    cur_user_p = get_object_or_404(UserProfile, user__id = cur_user.pk)
    context = ({'cur_user': cur_user, 'cur_user_p': cur_user_p,})
    return render(request, 'user_profiles/view.html', context)

def register(request):
    logout(request)
    if request.method == 'POST':
        form = UserCreationForm(request.POST)
        if form.is_valid():
            new_user = form.save()
            username = request.POST['username']
            password = request.POST['password1']
            user = authenticate(username=username, password=password)
            login(request, user)
            userp = UserProfile.objects.get(user_id = user.id)
            userp.about = request.POST['about']
            userp.website = request.POST['website']
            userp.save()
            return HttpResponseRedirect("/home/")
    else:
        form = UserCreationForm()
    return render(request, "user_profiles/register.html", {
        'form': form,
    })

@login_required
def edit(request):
    user_profile = UserProfile.objects.get(user__id = request.user.pk)
    if request.method == 'POST':
        form = UserProfileForm(request.POST, instance=user_profile)
        if form.is_valid():
            form.save()
            return HttpResponseRedirect("/home/")
    else:
        form = UserProfileForm(instance = user_profile)
    return render(request, "user_profiles/edit.html", {
        'form': form,
    })

@login_required
def profile(request): #Users own profile page
    cur_user_p = get_object_or_404(UserProfile, user__id = request.user.pk) #Get their profile
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

    return render(request, 'user_profiles/profile.html', context)
