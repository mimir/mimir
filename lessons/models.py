from django.db import models
from django.contrib.auth.models import User

# Create your models here.
class Lesson(models.Model):
    name = models.CharField(max_length = 100)
    tutorial = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    description = models.CharField(max_length = 400)
    @property
    def times_taken(self):
        return self.usertakeslesson_set.count() #TODO ensure this is efficient

    def __unicode__(self):
        return self.name

class Example(models.Model):
    lesson = models.ForeignKey(Lesson)
    problem = models.TextField()
    solution = models.TextField()
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    def __unicode__(self):
        return self.problem

class AnswerFormat(models.Model):
    name = models.CharField(max_length = 100)
    hint = models.CharField(max_length = 250)
    example = models.CharField(max_length = 250)
    regex = models.CharField(max_length = 300)
    def __unicode__(self):
        return self.name

class Question(models.Model):
    lesson = models.ForeignKey(Lesson)
    question = models.CharField(max_length = 300)
    answer = models.CharField(max_length = 200)
    answer_format = models.ForeignKey(AnswerFormat)
    calculation = models.TextField() #TODO work out how on earth this will work
    created = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    modified = models.DateTimeField(auto_now = True) #Modification date set on changing
    @property
    def times_answered(self):
        return self.useranswersquestion_set.count() #TODO ensure this is efficient
    #TODO add other useful queries here e.g. times answered correctly
    def __unicode__(self):
        return self.question

class LessonFollowsFromLesson(models.Model):
    leads_from = models.ForeignKey(Lesson, related_name='prerequisite')
    leads_to = models.ForeignKey(Lesson, related_name='postrequisite') #TODO think about these two related names, do they make sense?
    CHOICES = [(i,i) for i in range(11)] #0-10
    strength = models.PositiveSmallIntegerField(choices = CHOICES) #Not sure exactly how this will work yet TODO work it out. Roughly 10 should correspond to, you really need to know A to do B, and 0 should mean that they're sort of linked?
    def __unicode__(self):
        return self.leads_from.name + " leads to " + self.leads_to.name
