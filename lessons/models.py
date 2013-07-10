from django.db import models

# Create your models here.
class Lesson(models.Model):
    name = models.CharField(max_length = 100)
    tutorial = models.CharField(max_length = 1000)

class Question(models.Model):
    lesson = models.ForeignKey(Lesson)
    text = models.CharField(max_length = 300)
    answer = models.CharField(max_length = 200)
