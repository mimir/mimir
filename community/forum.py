from lessons.models import Lesson, Example, Question, LessonFollowsFromLesson
from user_profiles.models import UserProfile
from community.models import UserQuestion, UserAnswer, UserComment, UserRating
import json
from django.core import serializers
from django.contrib.auth.models import User
import datetime, time, calendar
import string
from django.utils.timezone import localtime

#Converts a list of JSON strings to a single JSON list
def listToJSON(list=[]):
    json_list = "["
    for item in list:
        json_list += item
        json_list += ", "
    json_list += "]"
    return json_list
   
#Creates a UNIX timestamp (seconds from epoch)   
def unix_time(dt):
    epoch = datetime.datetime.utcfromtimestamp(0)
    delta = dt - epoch
    return (delta.microseconds + (delta.seconds + delta.days * 24 * 3600) * 10**6) / 10**6
    #return delta.total_seconds()

#Creates a Javascript timestamp (milliseconds from epoch)
def unix_time_millis(dt):
    return unix_time(dt) * 1000.0

#Class to convert a UserQuestion object to JSON, cutting out useless data
class JSONUserQuestion:
    pk = 0
    lesson = ""
    question = ""
    answer = ""
    user_tag = "" #TODO: Replace with actual user tag system
    title = ""
    content = ""
    created = 0
    modified = 0
    rating = 0
    
    def __init__(self, question):
        gen_q = ["null", "null"]
        if question.question != None:
            #gen_q = generateQuestion(question.question_seed, question.question.question, question.question.calculation)
        
        self.pk = question.pk
        if question.lesson != None:
            self.lesson = question.lesson
        else:
            self.lesson = "null"
        self.question = gen_q[0]
        self.answer = gen_q[1]
        self.user_tag = question.user.username #UserProfile.objects.get(user = question.user.pk)
        self.title = question.title
        self.content = question.user_question
        #self.created = time.mktime(question.created.date().timetuple())*1000
        #self.modified = time.mktime(question.modified.date().timetuple())*1000
        self.created = unix_time_millis(question.created.replace(tzinfo=None))
        self.modified = unix_time_millis(question.modified.replace(tzinfo=None))
        self.rating = question.rating
        
    def toJSON(self):
        json = r'{"pk":%d, "lesson":"%s", "question":"%s", "answer":"%s", "user_tag":"%s", "title":"%s", "content":"%s", "created":%d, "modified":%d, "rating":%d}' % (self.pk, self.lesson, self.question, self.answer, self.user_tag, self.title, self.content, self.created, self.modified, self.rating)
        json = json.replace('\\', '\\\\').replace("\n", r"<\br>").replace("\r", r"")
        return json
        
    
#Class to convert a UserAnswer object to JSON, cutting out useless data
class JSONUserAnswer:
    user_tag = "" #TODO: Replace with actual user tag system
    content = ""
    created = 0
    modified = 0
    rating = 0
    
    def __init__(self, answer):
        self.user_tag = answer.user.username #UserProfile.objects.get(user = answer.user.pk)
        self.content = answer.user_answer
        self.created = unix_time_millis(answer.created.replace(tzinfo=None))
        self.modified = unix_time_millis(answer.modified.replace(tzinfo=None))
        self.rating = answer.rating
        
    def toJSON(self):
        json = r'{"user_tag":"%s", "content":"%s", "created":%d, "modified":%d, "rating":%d}' % (self.user_tag,  self.content, self.created, self.modified, self.rating)
        json = json.replace('\\', '\\\\').replace("\n", r"\n").replace("\r", r"\r")
        return json

#Class to convert a UserComment object to JSON, cutting out useless data
class JSONUserComment:
    user_tag = "" #TODO: Replace with actual user tag system
    content = ""
    created = 0
    modified = 0
    rating = 0
    
    def __init__(self, comment):
        self.user_tag = comment.user.username #UserProfile.objects.get(user = answer.user.pk)
        self.content = comment.user_comment
        self.created = unix_time_millis(comment.created.replace(tzinfo=None))
        self.modified = unix_time_millis(comment.modified.replace(tzinfo=None))
        self.rating = comment.rating
        
    def toJSON(self):
        json = r'{"user_tag":"%s", "content":"%s", "created":%d, "modified":%d, "rating":%d}' % (self.user_tag,  self.content, self.created, self.modified, self.rating)
        json = json.replace('\\', '\\\\').replace("\n", r"\n").replace("\r", r"\r")
        return json
