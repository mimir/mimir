from django.db import models
from django.contrib.auth.models import User
from lessons.models import Lesson, Question

# Create your models here.

class UserQuestion(models.Model): #When topics implemented make so that topic is mandatory but lesson isn't, that way allows misc question area
    lesson = models.ForeignKey(Lesson, blank=True, null=True)
    question = models.ForeignKey(Question, blank=True, null=True)
    question_seed = models.DecimalField(max_digits = 21, decimal_places = 20, blank=True, null=True)
    user = models.ForeignKey(User)
    title = models.CharField(max_length = 100, unique = True)
    user_question = models.TextField()
    rating = models.IntegerField(default = 0)
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing

    def __unicode__(self):
        return "Question - " + self.title

class UserAnswer(models.Model):
    user_question = models.ForeignKey(UserQuestion)
    user = models.ForeignKey(User)
    user_answer = models.TextField()
    rating = models.IntegerField(default = 0)
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing

    def __unicode__(self):
        return "Answer - " + self.user_question.title

class UserComment(models.Model):
    user_question = models.ForeignKey(UserQuestion, blank=True, null=True)
    user_answer = models.ForeignKey(UserAnswer, blank=True, null=True)
    user = models.ForeignKey(User)
    user_comment = models.TextField()
    rating = models.IntegerField(default = 0)
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing

    def __unicode__(self):
        return User.__unicode__(self.user) + " - " + self.user_comment #Should really change to the user's name field, whatever that is called.

class UserRating(models.Model): #When topics implemented make so that topic is mandatory but lesson isn't, that way allows misc question area
    user_question = models.ForeignKey(UserQuestion, blank=True, null=True)
    user_answer = models.ForeignKey(UserAnswer, blank=True, null=True)
    user_comment = models.ForeignKey(UserComment, blank=True, null=True)
    user = models.ForeignKey(User)
    rating = models.BooleanField()
    #created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    #modified = models.DateTimeField(auto_now = True) #Modification date set on changing

    def __unicode__(self):
        return self.user.name + " - " + self.user_question.title + " - " + self.rating
        
    class Meta:
        unique_together = ("user_question", "user_answer", "user_comment", "user")
