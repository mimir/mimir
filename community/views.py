from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, Http404, HttpResponseRedirect, HttpResponseServerError
from django.core.urlresolvers import reverse
from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson
#from lessons.generate import generateQuestion
from lessons.mas_main import create_question, create_solution
from user_profiles.models import UserProfile
from community.models import UserQuestion, UserAnswer, UserComment, UserRating
from community.forms import QuestionForm
import json
from django.core import serializers
from django.contrib.auth.models import User
from django.contrib.auth.decorators import login_required
from community.forum import JSONUserQuestion, JSONUserAnswer, JSONUserComment, listToJSON
from django.core.exceptions import ValidationError

def index(request):
    #Send list of all questions to page.
    #TODO: Send most recent, most popular, highest rated, etc via AJAX
    question_list = UserQuestion.objects.all()
    context = ({
        'question_list': question_list,
    })
    return render(request, 'community/index.html', context)

@login_required
def ask_question(request):
    if request.method == "POST":
        form = QuestionForm(request.POST)
        if form.is_valid():
            
            new_question = UserQuestion(lesson=form.cleaned_data['lesson'],question=form.cleaned_data['question'],question_seed=form.cleaned_data['question_seed'],title=form.cleaned_data['title'],user_question=form.cleaned_data['user_question'],rating = 0, user = request.user,)
            new_question.save()
            return HttpResponseRedirect(reverse('community:question', args=[new_question.id])) #TODO finish
    else:
        form = QuestionForm()
    return render(request, 'community/ask.html', {'form': form, })

def question(request, question_id):
    #Revamp of question to use templates over Javascript
    question = get_object_or_404(UserQuestion, pk = question_id)
    
    #Generates question-answer pair
    displayQuestion = []
    if question.question != None:
        displayQuestion = create_question(question.question_seed, question.question.question)
        displayAnswer = create_solution(question.question_seed, question.question.question, question.question.answer)
    
    q_comments = UserComment.objects.filter(user_question = question_id)
    answers = UserAnswer.objects.filter(user_question = question_id)
    a_comments = []
    for answer in answers:
        a_comments.append(UserComment.objects.filter(user_answer = answer.pk))
    
    context = ({'question': question, 'displayQuestion': displayQuestion, 'displayAnswer': displayAnswer, 'answers': answers, 'q_comments': q_comments, 'a_comments': a_comments, })
    return render(request, 'community/question.html', context)

def add_comment(request):
    if request.user.is_authenticated():
        p = request.POST
        if "comment" in p and "id" in p and "type" in p:
            id = int(p["id"])
            if p["type"] == "question":
                question = get_object_or_404(UserQuestion, pk=id)
                comment = UserComment(user=request.user, user_question = question, user_comment = p["comment"])
                comment.save()
            if p["type"] == "answer":
                answer = get_object_or_404(UserAnswer, pk=id)
                comment = UserComment(user=request.user, user_answer = answer, user_comment = p["comment"])
                comment.save()
    return HttpResponse('')

def add_answer(request):
    if request.user.is_authenticated():
        p = request.POST
        if "answer" in p and "id" in p:
            id = int(p["id"])
            question = get_object_or_404(UserQuestion, pk=id)
            answer = UserAnswer(user = request.user, user_question = question, user_answer = p["answer"])
            answer.save()
    return HttpResponse('')

def rate_user_item(request):
    if request.user.is_authenticated():
        p = request.POST
        if "rating" in p and "type" in p and "id" in p:
            user_item = None
            user_profile = None
            user_vote = None
            
            if p['type'] == 'Q':
                user_item = UserQuestion.objects.get(pk=p['id'])
            elif p['type'] == 'A':
                user_item = UserAnswer.objects.get(pk=p['id'])
            elif p['type'] == 'C':
                user_item = UserComment.objects.get(pk=p['id'])
            else:
                print "A type error occurred."
                return HttpResponseServerError("A type error occurred.")
            
            if user_item.user == request.user:
                return HttpResponseServerError("Can't rate own item.")

            if p['rating'] == 'up':
                user_item.rating += 1
                user_profile = UserProfile.objects.get(user=user_item.user)
                user_profile.reputation += 1
                user_vote = True
            elif p['rating'] == 'down':
                user_item.rating -= 1
                user_profile = UserProfile.objects.get(user=user_item.user)
                user_profile.reputation -= 1
                user_vote = False
            else:
                print "An invalid rating value error occurred."
                return HttpResponseServerError("An invalid rating value error occurred.")
            
            user_rating = None
            
            
            
            if p['type'] == 'Q':
                try:
                    user_rating = UserRating.objects.get(user=request.user, user_question=user_item)
                    return HttpResponse(content="You cannot rate something twice.", content_type="text/plaintext", status=500, reason="You cannot rate something twice.")    
                except:
                    pass
            
                user_rating = UserRating(user=request.user, user_question=user_item, rating=user_vote)
            elif p['type'] == 'A':
                try:
                    user_rating = UserRating.objects.get(user=request.user, user_answer=user_item)
                    return HttpResponse(content="You cannot rate something twice.", content_type="text/plaintext", status=500, reason="You cannot rate something twice.")   
                except:
                    pass
                    
                user_rating = UserRating(user=request.user, user_answer=user_item, rating=user_vote)
            elif p['type'] == 'C':
                try:
                    user_rating = UserRating.objects.get(user=request.user, user_comment=user_item)
                    return HttpResponse(content="You cannot rate something twice.", content_type="text/plaintext", status=500, reason="You cannot rate something twice.")  
                except:
                    pass
                    
                user_rating = UserRating(user=request.user, user_comment=user_item, rating=user_vote)
            
            try:
                user_rating.full_clean()
            except ValidationError as e:
                print "You can't rate something more than once."
                print e.message_dict
                return HttpResponse(content="You cannot rate something twice.", status=500)
            
            user_rating.save()
            user_item.save()
            user_profile.save()
            
        else:
            print "A POST request error occurred."
            return HttpResponseServerError("A POST request error occurred.")
    return HttpResponse('')

   



#Old version of JS DOM built questions
def jsQuestion(request, question_id):
    #Get question and convert to JSON
    question = get_object_or_404(UserQuestion, pk = question_id)
    json_question = JSONUserQuestion(question)
    
    #Get question comments and convert to JSON
    q_comments = list(UserComment.objects.filter(user_question = question_id))
    json_q_comments = []
    for comment in q_comments:
        json_comment = JSONUserComment(comment)
        json_q_comments.append(json_comment.toJSON())
    
    #Get answers and comments and convert to JSON
    answers = UserAnswer.objects.filter(user_question = question_id)
    json_answers = []
    json_a_comments = []
    answer_index = 0
    for answer in answers:
        json_answer = JSONUserAnswer(answer)
        json_answers.append(json_answer.toJSON())
        a_comments = UserComment.objects.filter(user_answer = answer.pk)
        json_comments = []
        for comment in a_comments:
            json_comment = JSONUserComment(comment)
            json_comments.append(json_comment.toJSON())
        json_a_comments.append(listToJSON(json_comments))
    
    #profile = UserProfile.objects.get(user = question.user.pk)
    
    
    context = ({'question': json_question.toJSON(), 'answers': listToJSON(json_answers), 'q_comments': listToJSON(json_q_comments), 'a_comments': listToJSON(json_a_comments), })
    return render(request, 'community/question.html', context)
#question(showing one question and the comments/answers associated with it)
#section(showing the most recent questions for either lesson, question, misc, etc)
#search(page to do searches and display results)?
